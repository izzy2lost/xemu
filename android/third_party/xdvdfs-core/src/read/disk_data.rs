use maybe_async::maybe_async;
use thiserror::Error;

use crate::layout::{DirectoryEntryDiskData, OutOfBounds};

#[non_exhaustive]
#[derive(Error, Debug, Copy, Clone, Eq, PartialEq)]
pub enum DiskDataReadError<E> {
    #[error("io error")]
    IOError(#[source] E),
    #[error("offset out of bounds")]
    SizeOutOfBounds(#[from] OutOfBounds),
}

impl DirectoryEntryDiskData {
    #[maybe_async]
    pub async fn read_data<BDR: crate::blockdev::BlockDeviceRead + ?Sized>(
        &self,
        dev: &mut BDR,
        buf: &mut [u8],
    ) -> Result<(), DiskDataReadError<BDR::ReadError>> {
        if self.data.size == 0 {
            return Ok(());
        }

        let offset = self.data.offset(0)?;
        dev.read(offset, buf)
            .await
            .map_err(DiskDataReadError::IOError)?;
        Ok(())
    }

    #[maybe_async]
    pub async fn read_data_all<BDR: crate::blockdev::BlockDeviceRead + ?Sized>(
        &self,
        dev: &mut BDR,
    ) -> Result<alloc::boxed::Box<[u8]>, DiskDataReadError<BDR::ReadError>> {
        self.read_data_offset(dev, self.data.size as u64, 0).await
    }

    #[maybe_async]
    pub async fn read_data_offset<BDR: crate::blockdev::BlockDeviceRead + ?Sized>(
        &self,
        dev: &mut BDR,
        size: u64,
        offset: u64,
    ) -> Result<alloc::boxed::Box<[u8]>, DiskDataReadError<BDR::ReadError>> {
        let size = core::cmp::min(size, self.data.size as u64);
        let buf = alloc::vec![0; size as usize];
        let mut buf = buf.into_boxed_slice();

        if self.data.size == 0 {
            return Ok(buf);
        }

        let offset = self.data.offset(offset)?;
        dev.read(offset, &mut buf)
            .await
            .map_err(DiskDataReadError::IOError)?;

        Ok(buf)
    }

    #[cfg(feature = "std")]
    pub fn seek_to(
        &self,
        seek: &mut impl std::io::Seek,
    ) -> Result<u64, DiskDataReadError<std::io::Error>> {
        use std::io::SeekFrom;

        let offset = self.data.offset(0)?;
        let offset = seek
            .seek(SeekFrom::Start(offset))
            .map_err(DiskDataReadError::IOError)?;
        Ok(offset)
    }
}

#[cfg(test)]
mod test {
    use crate::layout::{DirectoryEntryDiskData, DirentAttributes, DiskRegion};
    use futures::executor;

    #[test]
    fn test_layout_dirent_disk_data_read_data_empty() {
        let mut data: [u8; 8] = [1; 8];

        let dirent = DirectoryEntryDiskData {
            data: DiskRegion { sector: 0, size: 0 },
            attributes: DirentAttributes(0),
            filename_length: 0,
        };

        let mut buf = [0; 8];
        executor::block_on(dirent.read_data(data.as_mut_slice(), &mut buf)).unwrap();

        assert_eq!(buf, [0; 8]);
    }

    #[test]
    fn test_layout_dirent_disk_data_read_data_non_empty() {
        let mut data: [u8; 10] = [1; 10];

        let dirent = DirectoryEntryDiskData {
            data: DiskRegion {
                sector: 0,
                size: 10,
            },
            attributes: DirentAttributes(0),
            filename_length: 0,
        };

        let mut buf = [0; 8];
        executor::block_on(dirent.read_data(data.as_mut_slice(), &mut buf)).unwrap();

        assert_eq!(buf, [1; 8]);
    }

    #[test]
    fn test_layout_dirent_disk_data_read_data_all_empty() {
        let mut data: [u8; 8] = [1; 8];

        let dirent = DirectoryEntryDiskData {
            data: DiskRegion { sector: 0, size: 0 },
            attributes: DirentAttributes(0),
            filename_length: 0,
        };

        let data = executor::block_on(dirent.read_data_all(data.as_mut_slice())).unwrap();
        assert_eq!(data.len(), 0);
    }

    #[test]
    fn test_layout_dirent_disk_data_read_data_all_non_empty() {
        let mut data: [u8; 8] = [1; 8];

        let dirent = DirectoryEntryDiskData {
            data: DiskRegion { sector: 0, size: 8 },
            attributes: DirentAttributes(0),
            filename_length: 0,
        };

        let data = executor::block_on(dirent.read_data_all(data.as_mut_slice())).unwrap();
        assert_eq!(data.as_ref(), &[1; 8]);
    }

    #[test]
    fn test_layout_dirent_disk_data_read_data_offset_empty() {
        let mut data: [u8; 8] = [1; 8];

        let dirent = DirectoryEntryDiskData {
            data: DiskRegion { sector: 0, size: 0 },
            attributes: DirentAttributes(0),
            filename_length: 0,
        };

        let data = executor::block_on(dirent.read_data_offset(data.as_mut_slice(), 4, 2))
            .expect("Data should be read without error");
        assert_eq!(data.len(), 0);
    }

    #[test]
    fn test_layout_dirent_disk_data_read_data_offset_non_empty() {
        let mut data: [u8; 8] = [1, 2, 3, 4, 5, 6, 7, 8];

        let dirent = DirectoryEntryDiskData {
            data: DiskRegion { sector: 0, size: 8 },
            attributes: DirentAttributes(0),
            filename_length: 0,
        };

        let data = executor::block_on(dirent.read_data_offset(data.as_mut_slice(), 4, 2))
            .expect("Data should be read without error");
        assert_eq!(data.as_ref(), &[3, 4, 5, 6]);
    }
}

#[cfg(all(test, feature = "std"))]
mod test_std {
    use crate::layout::{DirectoryEntryDiskData, DirentAttributes, DiskRegion};
    use alloc::vec::Vec;
    use std::io::Cursor;

    #[test]
    fn test_layout_dirent_disk_data_seek_to() {
        let dirent = DirectoryEntryDiskData {
            data: DiskRegion { sector: 1, size: 2 },
            attributes: DirentAttributes(0),
            filename_length: 0,
        };

        let mut seeker = Cursor::new(Vec::new());
        let result = dirent.seek_to(&mut seeker).expect("Seek should succeed");
        assert_eq!(result, 2048);
    }
}
