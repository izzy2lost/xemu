#[cfg(not(feature = "sync"))]
use alloc::boxed::Box;

use maybe_async::maybe_async;

use crate::read::VolumeError;

use super::{BlockDeviceRead, BlockDeviceWrite};

/// Represents XGD types and their corresponding XDVDFS partition offsets.
///
/// These values are used to locate the start of the XDVDFS game partition within
/// images.
#[derive(Debug, Default, Copy, Clone, Eq, PartialEq)]
#[repr(u64)]
pub enum XDVDFSOffsets {
    #[default]
    XISO = 0,
    XGD1 = 405798912,
    XGD2 = 265879552,
    XGD3 = 34078720,
}

impl XDVDFSOffsets {
    const ALL: [XDVDFSOffsets; 4] = [
        XDVDFSOffsets::XISO,
        XDVDFSOffsets::XGD1,
        XDVDFSOffsets::XGD2,
        XDVDFSOffsets::XGD3,
    ];
}

impl From<XDVDFSOffsets> for u64 {
    fn from(x: XDVDFSOffsets) -> Self {
        x as u64
    }
}

pub struct OffsetWrapper<T>
where
    T: BlockDeviceRead + Sized,
{
    pub(crate) inner: T,
    pub(crate) offset: XDVDFSOffsets,
}

impl<T> OffsetWrapper<T>
where
    T: BlockDeviceRead + Sized,
{
    #[maybe_async]
    pub async fn new(dev: T) -> Result<Self, VolumeError<T::ReadError>> {
        let mut s = Self {
            inner: dev,
            offset: XDVDFSOffsets::default(),
        };

        for offset in XDVDFSOffsets::ALL {
            s.offset = offset;

            let vol = crate::read::read_volume(&mut s).await;
            if vol.is_ok() {
                return Ok(s);
            }
        }

        Err(VolumeError::InvalidVolume)
    }

    pub fn new_with_provided_offset(dev: T, offset: XDVDFSOffsets) -> Self {
        Self { inner: dev, offset }
    }

    pub fn get_offset(&self) -> XDVDFSOffsets {
        self.offset
    }

    pub fn get_ref(&self) -> &T {
        &self.inner
    }

    pub fn get_mut(&mut self) -> &mut T {
        &mut self.inner
    }
}

#[maybe_async]
impl<T> BlockDeviceRead for OffsetWrapper<T>
where
    T: BlockDeviceRead,
{
    type ReadError = T::ReadError;

    async fn read(&mut self, offset: u64, buffer: &mut [u8]) -> Result<(), Self::ReadError> {
        self.inner
            .read(offset + u64::from(self.offset), buffer)
            .await
    }
}

#[maybe_async]
impl<T> BlockDeviceWrite for OffsetWrapper<T>
where
    T: BlockDeviceRead + BlockDeviceWrite,
{
    type WriteError = <T as BlockDeviceWrite>::WriteError;

    async fn write(&mut self, offset: u64, buffer: &[u8]) -> Result<(), Self::WriteError> {
        self.inner
            .write(offset + u64::from(self.offset), buffer)
            .await
    }

    async fn len(&mut self) -> Result<u64, Self::WriteError> {
        self.inner.len().await
    }
}

#[cfg(feature = "std")]
impl<T> std::io::Seek for OffsetWrapper<T>
where
    T: BlockDeviceRead + std::io::Seek,
{
    fn seek(&mut self, pos: std::io::SeekFrom) -> std::io::Result<u64> {
        use std::io::SeekFrom;
        match pos {
            SeekFrom::Start(pos) => self
                .inner
                .seek(SeekFrom::Start(u64::from(self.offset) + pos)),
            pos => self.inner.seek(pos),
        }
    }
}

#[cfg(all(test, feature = "write"))]
mod test {
    use futures::executor::block_on;

    use crate::{
        blockdev::{BlockDeviceRead, BlockDeviceWrite, XDVDFSOffsets},
        layout::{DirectoryEntryTable, DiskRegion, VolumeDescriptor},
        read::VolumeError,
        write::fs::{
            MemoryFilesystem, SectorLinearBlockDevice, SectorLinearBlockFilesystem,
            SectorLinearImage,
        },
    };

    use super::OffsetWrapper;

    #[test]
    fn test_blockdev_offset_wrapper_create_invalid_image() {
        let slbd = SectorLinearBlockDevice::default();
        let mut fs = SectorLinearBlockFilesystem::new(MemoryFilesystem::default());
        let image = SectorLinearImage::new(&slbd, &mut fs);

        let res = block_on(OffsetWrapper::new(image)).err();
        assert_eq!(res, Some(VolumeError::InvalidVolume));
    }

    #[test]
    fn test_blockdev_offset_wrapper_create_from_xiso() {
        let mut slbd = SectorLinearBlockDevice::default();
        let mut fs = SectorLinearBlockFilesystem::new(MemoryFilesystem::default());

        let volume = VolumeDescriptor::new(DirectoryEntryTable {
            region: DiskRegion { sector: 0, size: 0 },
        });
        let volume = volume
            .serialize()
            .expect("Volume serialization should succeed");
        block_on(slbd.write(32 * 2048, &volume)).expect("Write should succeed");

        let image = SectorLinearImage::new(&slbd, &mut fs);
        let mut wrapper =
            block_on(OffsetWrapper::new(image)).expect("Offset wrapper creation should succeed");
        assert_eq!(wrapper.get_offset() as u64, 0);

        let mut vol_read = [0u8; 2048];
        block_on(wrapper.read(32 * 2048, &mut vol_read)).expect("Volume read should succeed");
        assert_eq!(volume, vol_read);
    }

    #[test]
    fn test_blockdev_offset_wrapper_create_from_xgd1() {
        let mut slbd = SectorLinearBlockDevice::default();
        let mut fs = SectorLinearBlockFilesystem::new(MemoryFilesystem::default());

        let volume = VolumeDescriptor::new(DirectoryEntryTable {
            region: DiskRegion { sector: 0, size: 0 },
        });
        let volume = volume
            .serialize()
            .expect("Volume serialization should succeed");

        let xgd1_volume_header_offset = 405798912 + 32 * 2048;
        block_on(slbd.write(xgd1_volume_header_offset, &volume)).expect("Write should succeed");

        let image = SectorLinearImage::new(&slbd, &mut fs);
        let mut wrapper =
            block_on(OffsetWrapper::new(image)).expect("Offset wrapper creation should succeed");
        assert_eq!(wrapper.get_offset(), XDVDFSOffsets::XGD1);

        let mut vol_read = [0u8; 2048];
        block_on(wrapper.read(32 * 2048, &mut vol_read)).expect("Volume read should succeed");
        assert_eq!(volume, vol_read);
    }

    #[test]
    fn test_blockdev_offset_wrapper_create_from_xgd2() {
        let mut slbd = SectorLinearBlockDevice::default();
        let mut fs = SectorLinearBlockFilesystem::new(MemoryFilesystem::default());

        let volume = VolumeDescriptor::new(DirectoryEntryTable {
            region: DiskRegion { sector: 0, size: 0 },
        });
        let volume = volume
            .serialize()
            .expect("Volume serialization should succeed");

        let xgd2_volume_header_offset = 265879552 + 32 * 2048;
        block_on(slbd.write(xgd2_volume_header_offset, &volume)).expect("Write should succeed");

        let image = SectorLinearImage::new(&slbd, &mut fs);
        let mut wrapper =
            block_on(OffsetWrapper::new(image)).expect("Offset wrapper creation should succeed");
        assert_eq!(wrapper.get_offset(), XDVDFSOffsets::XGD2);

        let mut vol_read = [0u8; 2048];
        block_on(wrapper.read(32 * 2048, &mut vol_read)).expect("Volume read should succeed");
        assert_eq!(volume, vol_read);
    }

    #[test]
    fn test_blockdev_offset_wrapper_create_from_xgd3() {
        let mut slbd = SectorLinearBlockDevice::default();
        let mut fs = SectorLinearBlockFilesystem::new(MemoryFilesystem::default());

        let volume = VolumeDescriptor::new(DirectoryEntryTable {
            region: DiskRegion { sector: 0, size: 0 },
        });
        let volume = volume
            .serialize()
            .expect("Volume serialization should succeed");

        let xgd3_volume_header_offset = 34078720 + 32 * 2048;
        block_on(slbd.write(xgd3_volume_header_offset, &volume)).expect("Write should succeed");

        let image = SectorLinearImage::new(&slbd, &mut fs);
        let mut wrapper =
            block_on(OffsetWrapper::new(image)).expect("Offset wrapper creation should succeed");
        assert_eq!(wrapper.get_offset(), XDVDFSOffsets::XGD3);

        let mut vol_read = [0u8; 2048];
        block_on(wrapper.read(32 * 2048, &mut vol_read)).expect("Volume read should succeed");
        assert_eq!(volume, vol_read);
    }

    #[test]
    fn test_blockdev_offset_wrapper_write() {
        let mut slbd = SectorLinearBlockDevice::default();
        let mut fs = SectorLinearBlockFilesystem::new(MemoryFilesystem::default());
        let image = SectorLinearImage::new(&mut slbd, &mut fs);
        let mut wrapper = OffsetWrapper::new_with_provided_offset(image, XDVDFSOffsets::XGD1);

        let data = [1, 2, 3, 4, 5];
        block_on(wrapper.write(2048, &data)).expect("Write should succeed");

        let mut buffer = [0u8; 2048];
        block_on(wrapper.get_mut().read(405800960, &mut buffer)).expect("Read should succeed");
        assert_eq!(buffer[0..5], data);

        let len = block_on(wrapper.len()).expect("Reading length should work");
        assert_eq!(len, 405803008);
    }
}

#[cfg(all(test, feature = "std"))]
mod test_std {
    use alloc::vec::Vec;
    use std::io::{Cursor, Seek, SeekFrom};

    use crate::blockdev::XDVDFSOffsets;

    use super::OffsetWrapper;

    #[test]
    fn test_blockdev_offset_wrapper_seek() {
        let seeker = Cursor::new(Vec::new());
        let mut wrapper = OffsetWrapper::new_with_provided_offset(seeker, XDVDFSOffsets::XGD1);

        let res = wrapper
            .seek(SeekFrom::Start(12345))
            .expect("Seek should succeed");
        assert_eq!(res, 405811257);
        assert_eq!(wrapper.get_ref().position(), 405811257);

        let res = wrapper
            .seek(SeekFrom::Current(-45))
            .expect("Seek should succeed");
        assert_eq!(res, 405811212);
        assert_eq!(wrapper.get_ref().position(), 405811212);
    }
}
