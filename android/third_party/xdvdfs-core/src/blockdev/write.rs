#[cfg(not(feature = "sync"))]
use alloc::boxed::Box;

use maybe_async::maybe_async;

/// Trait for write operations on some block device
///
/// Calls to trait methods will always be thread safe (that is, no two calls within the trait will
/// be made on the same blockdevice at the same time)
#[maybe_async]
pub trait BlockDeviceWrite: Send + Sync {
    type WriteError: core::error::Error + Send + Sync + 'static;

    async fn write(&mut self, offset: u64, buffer: &[u8]) -> Result<(), Self::WriteError>;
    async fn len(&mut self) -> Result<u64, Self::WriteError>;

    async fn is_empty(&mut self) -> Result<bool, Self::WriteError> {
        self.len().await.map(|len| len == 0)
    }
}

#[cfg(feature = "std")]
#[maybe_async]
impl<W> BlockDeviceWrite for W
where
    W: std::io::Write + std::io::Seek + Send + Sync,
{
    type WriteError = std::io::Error;

    async fn write(&mut self, offset: u64, buffer: &[u8]) -> Result<(), std::io::Error> {
        self.seek(std::io::SeekFrom::Start(offset))?;
        std::io::Write::write_all(self, buffer)?;

        Ok(())
    }

    async fn len(&mut self) -> Result<u64, std::io::Error> {
        use std::io::SeekFrom;

        let current_position = self.stream_position()?;
        let len = self.seek(SeekFrom::End(0))?;
        self.seek(SeekFrom::Start(current_position))?;

        Ok(len)
    }
}

#[cfg(all(test, feature = "std"))]
mod test_io_impl {
    use alloc::vec::Vec;
    use std::io::Cursor;

    use futures::executor::block_on;

    use super::BlockDeviceWrite;

    #[test]
    fn test_blockdev_write_std_read_impl() {
        let mut cursor = Cursor::new(Vec::new());
        let buf = [1, 2, 3, 4];

        let res = block_on(BlockDeviceWrite::is_empty(&mut cursor));
        assert!(res.is_ok_and(|empty| empty));

        let res = block_on(BlockDeviceWrite::write(&mut cursor, 0, &buf));
        assert!(res.is_ok());
        assert_eq!(cursor.get_ref(), &[1, 2, 3, 4]);

        let res = block_on(BlockDeviceWrite::write(&mut cursor, 2, &buf));
        assert!(res.is_ok());
        assert_eq!(cursor.get_ref(), &[1, 2, 1, 2, 3, 4]);

        let len = block_on(BlockDeviceWrite::len(&mut cursor));
        assert!(len.is_ok_and(|len| len == 6));
    }
}
