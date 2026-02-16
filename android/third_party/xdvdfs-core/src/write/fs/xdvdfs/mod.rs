use alloc::vec::Vec;

use thiserror::Error;

#[cfg(not(feature = "sync"))]
use alloc::boxed::Box;

use maybe_async::maybe_async;

use crate::blockdev::{BlockDeviceRead, BlockDeviceWrite};
use crate::layout::DirectoryEntryNode;
use crate::read::DirectoryTableLookupError;

use super::FilesystemCopier;
use super::FilesystemHierarchy;
use super::PathRef;
use super::{FileEntry, FileType, PathPrefixTree};

mod copier;
pub use copier::*;

/// Error type for XDVDFSFilesystem operations
/// A BlockDev error is an error that occurred during a copy operation
/// in the respective block device side.
/// A FilesystemReadErr is an error that occurred while traversing the
/// underlying XDVDFS filesystem.
#[derive(Error, Debug, Eq, PartialEq)]
pub enum XDVDFSFilesystemError<ReadErr, WriteErr> {
    #[error("failed to read xdvdfs filesystem")]
    FilesystemReadErr(#[source] DirectoryTableLookupError<ReadErr>),
    #[error("failed to read from block device")]
    BlockDevReadErr(#[source] ReadErr),
    #[error("failed to write to block device")]
    BlockDevWriteErr(#[source] WriteErr),
}

/// A Filesystem backed by an XDVDFS block device
/// Reads entries and data from a supplied XDVDFS image
pub struct XDVDFSFilesystem<D, W, Copier = DefaultCopier<D, W>>
where
    D: BlockDeviceRead + Sized,
    W: BlockDeviceWrite + ?Sized,
{
    dev: D,
    volume: crate::layout::VolumeDescriptor,
    dirent_cache: PathPrefixTree<DirectoryEntryNode>,
    copier: Copier,

    w_type: core::marker::PhantomData<W>,
}

impl<D, W, Copier> XDVDFSFilesystem<D, W, Copier>
where
    D: BlockDeviceRead + Sized,
    W: BlockDeviceWrite + ?Sized,
    Copier: RWCopier<D, W>,
{
    #[maybe_async]
    pub async fn new(mut dev: D) -> Option<XDVDFSFilesystem<D, W, Copier>> {
        let volume = crate::read::read_volume(&mut dev).await;

        if let Ok(volume) = volume {
            Some(Self {
                dev,
                volume,
                dirent_cache: PathPrefixTree::default(),
                copier: Copier::default(),
                w_type: core::marker::PhantomData,
            })
        } else {
            None
        }
    }
}

#[maybe_async]
impl<D, W, Copier> FilesystemHierarchy for XDVDFSFilesystem<D, W, Copier>
where
    D: BlockDeviceRead + Sized,
    W: BlockDeviceWrite + ?Sized,
    Copier: RWCopier<D, W> + Send + Sync,
{
    type Error = DirectoryTableLookupError<D::ReadError>;

    async fn read_dir(&mut self, dir: PathRef<'_>) -> Result<Vec<FileEntry>, Self::Error> {
        let (dirtab, cache_node) = if dir.is_root() {
            (self.volume.root_table, &mut self.dirent_cache)
        } else {
            // FIXME: This lookup does not work if `dir` has not been previously
            // found by this function. That is, `dir`'s parent needs to have been queried
            // before `dir` can be queried. This assumption is valid currently as `read_dir`
            // is only used to recursively scan directory contents, but the function contract
            // does not guarantee it generally.
            let (dirent, node) = self
                .dirent_cache
                .lookup_node_mut(dir)
                .and_then(|node| node.record.as_mut())
                .map(|(dirent, subtree)| (*dirent, subtree.as_mut()))
                .ok_or(DirectoryTableLookupError::NoDirent)?;
            let dirtab = dirent
                .node
                .dirent
                .dirent_table()
                .ok_or(DirectoryTableLookupError::IsNotDirectory)?;
            (dirtab, node)
        };

        let mut tree = dirtab.scan_dirent_tree(&mut self.dev).await?;
        let mut entries = Vec::new();
        while let Some(dirent) = tree.next_entry().await? {
            let name_str = dirent.name_str()?;
            cache_node.insert_tail(&name_str, dirent);
            entries.push(FileEntry {
                name: name_str.into_owned(),
                file_type: if dirent.node.dirent.is_directory() {
                    FileType::Directory
                } else {
                    FileType::File
                },
                len: dirent.node.dirent.data.size as u64,
            });
        }

        Ok(entries)
    }

    async fn clear_cache(&mut self) -> Result<(), Self::Error> {
        self.dirent_cache = PathPrefixTree::default();

        Ok(())
    }
}

#[maybe_async]
impl<D, W, Copier> FilesystemCopier<W> for XDVDFSFilesystem<D, W, Copier>
where
    D: BlockDeviceRead + Sized,
    W: BlockDeviceWrite + ?Sized,
    Copier: RWCopier<D, W> + Send + Sync,
{
    type Error = XDVDFSFilesystemError<D::ReadError, W::WriteError>;

    async fn copy_file_in(
        &mut self,
        src: PathRef<'_>,
        dest: &mut W,
        input_offset: u64,
        output_offset: u64,
        size: u64,
    ) -> Result<u64, Self::Error> {
        let dirent = self
            .dirent_cache
            .get(src)
            .ok_or(DirectoryTableLookupError::NoDirent)
            .map_err(XDVDFSFilesystemError::FilesystemReadErr)?;

        let size_to_copy = core::cmp::min(size, dirent.node.dirent.data.size as u64);
        if size_to_copy == 0 {
            return Ok(0);
        }

        let input_offset = dirent
            .node
            .dirent
            .data
            .offset(input_offset)
            .map_err(DirectoryTableLookupError::SizeOutOfBounds)
            .map_err(XDVDFSFilesystemError::FilesystemReadErr)?;
        self.copier
            .copy(
                input_offset,
                output_offset,
                size_to_copy,
                &mut self.dev,
                dest,
            )
            .await
    }
}

#[cfg(test)]
mod test {
    use futures::executor::block_on;

    use crate::{
        blockdev::NullBlockDevice,
        write::{
            fs::{
                FileEntry, FileType, FilesystemCopier, FilesystemHierarchy, MemoryFilesystem,
                SectorLinearBlockDevice, SectorLinearBlockFilesystem, SectorLinearImage,
            },
            img::{create_xdvdfs_image, NoOpProgressVisitor},
        },
    };

    use super::{DefaultCopier, XDVDFSFilesystem};

    #[test]
    fn test_write_xdvdfs_invalid_volume() {
        let memfs = MemoryFilesystem::default();
        let mut slbdfs = SectorLinearBlockFilesystem::new(memfs);
        let slbd = SectorLinearBlockDevice::default();
        let img = SectorLinearImage::new(&slbd, &mut slbdfs);
        let fs = block_on(XDVDFSFilesystem::<_, NullBlockDevice, DefaultCopier<_, _>>::new(img));

        assert!(fs.is_none());
    }

    #[test]
    fn test_write_xdvdfs_hierarchy() {
        let mut memfs = MemoryFilesystem::default();
        memfs.create("/a/b/c", b"Hello World");
        memfs.create("/a/d", b"Goodbye World");
        let mut slbdfs = SectorLinearBlockFilesystem::new(memfs);
        let mut slbd = SectorLinearBlockDevice::default();

        block_on(create_xdvdfs_image(
            &mut slbdfs,
            &mut slbd,
            NoOpProgressVisitor,
        ))
        .expect("Image creation should succeed");
        let img = SectorLinearImage::new(&slbd, &mut slbdfs);
        let mut fs =
            block_on(XDVDFSFilesystem::<_, NullBlockDevice, DefaultCopier<_, _>>::new(img))
                .expect("xdvdfs filesystem init should succeed");

        let dir = block_on(fs.read_dir("/".into())).expect("Root read should succeed");
        assert_eq!(
            dir,
            &[FileEntry {
                name: "a".into(),
                file_type: FileType::Directory,
                len: 2048,
            },]
        );

        let dir = block_on(fs.read_dir("/a".into())).expect("/a read should succeed");
        assert_eq!(
            dir,
            &[
                FileEntry {
                    name: "b".into(),
                    file_type: FileType::Directory,
                    len: 2048,
                },
                FileEntry {
                    name: "d".into(),
                    file_type: FileType::File,
                    len: 13,
                },
            ]
        );

        let dir = block_on(fs.read_dir("/a/b".into())).expect("/a/b read should succeed");
        assert_eq!(
            dir,
            &[FileEntry {
                name: "c".into(),
                file_type: FileType::File,
                len: 11,
            },]
        );
    }

    #[test]
    fn test_write_xdvdfs_copier() {
        let mut memfs = MemoryFilesystem::default();
        memfs.create("/a/b/c", b"Hello World");
        memfs.create("/a/d", b"Goodbye World");
        let mut slbdfs = SectorLinearBlockFilesystem::new(memfs);
        let mut slbd = SectorLinearBlockDevice::default();

        block_on(create_xdvdfs_image(
            &mut slbdfs,
            &mut slbd,
            NoOpProgressVisitor,
        ))
        .expect("Image creation should succeed");
        let img = SectorLinearImage::new(&slbd, &mut slbdfs);
        let mut fs = block_on(XDVDFSFilesystem::<_, [u8], DefaultCopier<_, _>>::new(img))
            .expect("xdvdfs filesystem init should succeed");

        // The cache must be populated by `read_dir` before the copier can access files
        block_on(fs.read_dir("/".into())).expect("Root read should succeed");
        block_on(fs.read_dir("/a".into())).expect("/a read should succeed");
        let dir = block_on(fs.read_dir("/a/b".into())).expect("/a/b read should succeed");
        assert_eq!(
            dir,
            &[FileEntry {
                name: "c".into(),
                file_type: FileType::File,
                len: 11,
            },]
        );

        let mut buf = [0u8; 9];
        let copied = block_on(fs.copy_file_in("/a/b/c".into(), &mut buf, 4, 2, 7));
        assert_eq!(copied, Ok(7));
        assert_eq!(&buf, b"\0\0o World");
    }

    #[test]
    fn test_write_xdvdfs_cache_clear() {
        let mut memfs = MemoryFilesystem::default();
        memfs.create("/a/b/c", b"Hello World");
        memfs.create("/a/d", b"Goodbye World");
        let mut slbdfs = SectorLinearBlockFilesystem::new(memfs);
        let mut slbd = SectorLinearBlockDevice::default();

        block_on(create_xdvdfs_image(
            &mut slbdfs,
            &mut slbd,
            NoOpProgressVisitor,
        ))
        .expect("Image creation should succeed");
        let img = SectorLinearImage::new(&slbd, &mut slbdfs);
        let mut fs = block_on(XDVDFSFilesystem::<_, [u8], DefaultCopier<_, _>>::new(img))
            .expect("xdvdfs filesystem init should succeed");

        block_on(fs.read_dir("/".into())).expect("Root read should succeed");
        block_on(fs.read_dir("/a".into())).expect("/a read should succeed");
        let dir = block_on(fs.read_dir("/a/b".into())).expect("/a/b read should succeed");
        assert_eq!(
            dir,
            &[FileEntry {
                name: "c".into(),
                file_type: FileType::File,
                len: 11,
            },]
        );

        let res = block_on(fs.clear_cache());
        assert_eq!(res, Ok(()));
        assert_eq!(fs.dirent_cache.get("a"), None);
    }
}
