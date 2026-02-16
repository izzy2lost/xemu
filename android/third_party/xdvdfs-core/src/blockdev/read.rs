#[cfg(not(feature = "sync"))]
use alloc::boxed::Box;

use maybe_async::maybe_async;

/// Trait for read operations on some block device containing an XDVDFS filesystem
///
/// Calls to `read` will always be thread safe (that is, no two calls to `read` will
/// be made on the same blockdevice at the same time)
#[maybe_async]
pub trait BlockDeviceRead: Send + Sync {
    type ReadError: core::error::Error + Send + Sync + 'static;

    async fn read(&mut self, offset: u64, buffer: &mut [u8]) -> Result<(), Self::ReadError>;
}

#[cfg(feature = "std")]
#[maybe_async]
impl<R> BlockDeviceRead for R
where
    R: std::io::Read + std::io::Seek + Send + Sync,
{
    type ReadError = std::io::Error;

    async fn read(&mut self, offset: u64, buffer: &mut [u8]) -> Result<(), std::io::Error> {
        self.seek(std::io::SeekFrom::Start(offset))?;
        std::io::Read::read_exact(self, buffer)?;

        Ok(())
    }
}

#[cfg(all(test, feature = "std"))]
mod test_io_impl {
    use std::io::Cursor;

    use futures::executor::block_on;

    use super::BlockDeviceRead;

    #[test]
    fn test_blockdev_read_std_read_impl() {
        let mut cursor = Cursor::new(&[1, 2, 3, 4, 5]);
        let mut buf = [0, 0, 0];

        let res = block_on(BlockDeviceRead::read(&mut cursor, 1, &mut buf));
        assert!(res.is_ok());
        assert_eq!(buf, [2, 3, 4]);
    }
}
