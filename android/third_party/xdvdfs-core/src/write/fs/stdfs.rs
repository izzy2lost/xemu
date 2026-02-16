use alloc::borrow::ToOwned;
use alloc::vec::Vec;
use maybe_async::maybe_async;
use std::{
    fs::DirEntry,
    path::{Path, PathBuf},
};

#[cfg(not(feature = "sync"))]
use alloc::boxed::Box;

use crate::blockdev::{BlockDeviceWrite, NullBlockDevice};

use super::{FileEntry, FileType, FilesystemCopier, FilesystemHierarchy, PathRef};

pub struct StdFilesystem {
    root: PathBuf,
}

impl StdFilesystem {
    pub fn create(root: &Path) -> Self {
        Self {
            root: root.to_owned(),
        }
    }

    fn direntry_to_file_entry(dir_entry: std::io::Result<DirEntry>) -> std::io::Result<FileEntry> {
        use std::io::{Error, ErrorKind};
        use std::string::ToString;

        let dir_entry = dir_entry?;

        let file_type = dir_entry.file_type()?;
        let file_type = if file_type.is_dir() {
            FileType::Directory
        } else if file_type.is_file() {
            FileType::File
        } else {
            return Err(Error::from(ErrorKind::Unsupported));
        };

        let name = dir_entry
            .file_name()
            .to_str()
            .map(|s| s.to_string())
            .ok_or(Error::from(ErrorKind::Unsupported))?;

        let len = match file_type {
            FileType::File => dir_entry.metadata()?.len(),
            FileType::Directory => 0,
        };

        Ok(FileEntry {
            name,
            file_type,
            len,
        })
    }
}

#[maybe_async]
impl FilesystemHierarchy for StdFilesystem {
    type Error = std::io::Error;

    async fn read_dir(&mut self, dir: PathRef<'_>) -> Result<Vec<FileEntry>, std::io::Error> {
        let dir = dir.as_path_buf(&self.root);
        let listing: std::io::Result<Vec<FileEntry>> = std::fs::read_dir(dir)?
            .map(Self::direntry_to_file_entry)
            .collect();

        listing
    }
}

#[maybe_async]
impl<T> FilesystemCopier<T> for StdFilesystem
where
    T: std::io::Write + std::io::Seek + BlockDeviceWrite<WriteError = std::io::Error>,
{
    type Error = std::io::Error;

    async fn copy_file_in(
        &mut self,
        src: PathRef<'_>,
        dest: &mut T,
        input_offset: u64,
        output_offset: u64,
        size: u64,
    ) -> Result<u64, std::io::Error> {
        use std::io::{Read, Seek, SeekFrom};

        dest.seek(SeekFrom::Start(output_offset))?;

        let src = src.as_path_buf(&self.root);
        let mut file = std::fs::File::open(src)?;
        file.seek(SeekFrom::Start(input_offset))?;

        let mut file = std::io::BufReader::with_capacity(1024 * 1024, file.take(size));
        std::io::copy(&mut file, dest)
    }
}

#[maybe_async]
impl FilesystemCopier<[u8]> for StdFilesystem {
    type Error = std::io::Error;

    async fn copy_file_in(
        &mut self,
        src: PathRef<'_>,
        dest: &mut [u8],
        input_offset: u64,
        output_offset: u64,
        size: u64,
    ) -> Result<u64, std::io::Error> {
        use std::io::{Read, Seek, SeekFrom};

        let src = src.as_path_buf(&self.root);
        let file = std::fs::File::open(src)?;
        let mut file = std::io::BufReader::with_capacity(1024 * 1024, file);
        file.seek(SeekFrom::Start(input_offset))?;

        let output_offset = output_offset as usize;
        let size = core::cmp::min(size as usize, <[u8]>::len(dest) - output_offset);

        let dest = &mut dest[output_offset..(output_offset + size)];
        let bytes_read = Read::read(&mut file, dest)?;
        dest[bytes_read..].fill(0);
        Ok(<[u8]>::len(dest) as u64)
    }
}

#[maybe_async]
impl FilesystemCopier<NullBlockDevice> for StdFilesystem {
    type Error = core::convert::Infallible;

    async fn copy_file_in(
        &mut self,
        _src: PathRef<'_>,
        dest: &mut NullBlockDevice,
        _input_offset: u64,
        output_offset: u64,
        size: u64,
    ) -> Result<u64, core::convert::Infallible> {
        dest.write_size_adjustment(output_offset, size);
        Ok(size)
    }
}

#[cfg(test)]
mod test {
    use alloc::vec::Vec;
    use std::io::{Cursor, Write};

    use futures::executor::block_on;

    use crate::{
        blockdev::NullBlockDevice,
        write::fs::{FileEntry, FileType, FilesystemCopier, FilesystemHierarchy},
    };

    use super::StdFilesystem;

    #[test]
    fn test_write_stdfs_read_dir() {
        let tempdir = tempfile::tempdir().expect("Failed to create tempdir");

        std::fs::create_dir(tempdir.path().join("subdir")).expect("Failed to create dir direntry");
        std::fs::create_dir(tempdir.path().join("subdir/entry"))
            .expect("Failed to create dir direntry");

        {
            let mut file = std::fs::File::create(tempdir.path().join("file"))
                .expect("Failed to create file direntry");
            file.write_all(b"Hello World")
                .expect("Failed to write to file entry");
        }

        let mut stdfs = StdFilesystem::create(tempdir.path());

        let mut result = block_on(stdfs.read_dir("/".into())).expect("read_dir should succeed");

        // Filesystem ordering is non-deterministic,
        // so sort the records by name.
        result.sort();
        assert_eq!(
            result,
            &[
                FileEntry {
                    name: "file".into(),
                    file_type: FileType::File,
                    len: 11,
                },
                FileEntry {
                    name: "subdir".into(),
                    file_type: FileType::Directory,
                    len: 0,
                },
            ],
        );

        let result = block_on(stdfs.read_dir("/subdir".into())).expect("read_dir should succeed");
        assert_eq!(
            result,
            &[FileEntry {
                name: "entry".into(),
                file_type: FileType::Directory,
                len: 0,
            },],
        );
    }

    #[test]
    fn test_write_stdfs_io_write_copier() {
        let tempdir = tempfile::tempdir().expect("Failed to create tempdir");

        std::fs::create_dir(tempdir.path().join("subdir")).expect("Failed to create dir direntry");

        {
            let mut file = std::fs::File::create(tempdir.path().join("subdir/file"))
                .expect("Failed to create file direntry");
            file.write_all(b"Hello World")
                .expect("Failed to write to file entry");
        }

        let mut stdfs = StdFilesystem::create(tempdir.path());
        let mut cursor = Cursor::new(Vec::<u8>::new());

        let result = block_on(stdfs.copy_file_in(
            "subdir/file".into(),
            &mut cursor,
            /*input_offset=*/ 1,
            /*output_offset=*/ 2,
            /*size=*/ 10,
        ));
        assert!(result.is_ok_and(|len| len == 10));
        assert_eq!(cursor.get_ref(), b"\0\0ello World");
    }

    #[test]
    fn test_write_stdfs_byte_slice_copier() {
        let tempdir = tempfile::tempdir().expect("Failed to create tempdir");

        std::fs::create_dir(tempdir.path().join("subdir")).expect("Failed to create dir direntry");

        {
            let mut file = std::fs::File::create(tempdir.path().join("subdir/file"))
                .expect("Failed to create file direntry");
            file.write_all(b"Hello World")
                .expect("Failed to write to file entry");
        }

        let mut stdfs = StdFilesystem::create(tempdir.path());
        let mut buffer = [0u8; 13];

        let result = block_on(stdfs.copy_file_in(
            "subdir/file".into(),
            buffer.as_mut_slice(),
            /*input_offset=*/ 1,
            /*output_offset=*/ 2,
            /*size=*/ 10,
        ));
        assert!(result.is_ok_and(|len| len == 10));
        assert_eq!(&buffer, b"\0\0ello World\0");
    }

    #[test]
    fn test_write_stdfs_null_copier() {
        let tempdir = tempfile::tempdir().expect("Failed to create tempdir");

        std::fs::create_dir(tempdir.path().join("subdir")).expect("Failed to create dir direntry");

        {
            let mut file = std::fs::File::create(tempdir.path().join("subdir/file"))
                .expect("Failed to create file direntry");
            file.write_all(b"Hello World")
                .expect("Failed to write to file entry");
        }

        let mut stdfs = StdFilesystem::create(tempdir.path());
        let mut nullbd = NullBlockDevice::default();

        let result = block_on(stdfs.copy_file_in(
            "subdir/file".into(),
            &mut nullbd,
            /*input_offset=*/ 1,
            /*output_offset=*/ 2,
            /*size=*/ 10,
        ));
        assert!(result.is_ok_and(|len| len == 10));
        assert_eq!(nullbd.len_blocking(), 12);
    }
}
