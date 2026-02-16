use maybe_async::maybe_async;
use thiserror::Error;

use crate::blockdev::BlockDeviceRead;
use crate::layout::{VolumeDescriptor, SECTOR_SIZE_U64};

#[non_exhaustive]
#[derive(Error, Debug, Copy, Clone, Eq, PartialEq)]
pub enum VolumeError<E> {
    #[error("io error")]
    IOError(#[from] E),
    #[error("not an xdvdfs volume")]
    InvalidVolume,
}

/// Read the XDVDFS volume descriptor from sector 32 of the drive
/// Returns None if the volume descriptor is invalid
#[maybe_async]
pub async fn read_volume<BDR: BlockDeviceRead + ?Sized>(
    dev: &mut BDR,
) -> Result<VolumeDescriptor, VolumeError<BDR::ReadError>> {
    let mut buffer = [0; core::mem::size_of::<VolumeDescriptor>()];

    // FIXME: Implement some form of check to whether the IO
    // error is a real error, or just indicates the volume is invalid
    // (i.e. if the disk is not large enough to fit a volume descriptor)
    // and propagate up the real error independently.
    // For now just assume any read error means the volume is invalid.
    dev.read(32 * SECTOR_SIZE_U64, &mut buffer)
        .await
        .map_err(|_| VolumeError::InvalidVolume)?;

    VolumeDescriptor::deserialize(&buffer)
        .ok()
        .filter(VolumeDescriptor::is_valid)
        .ok_or(VolumeError::InvalidVolume)
}

#[cfg(all(test, feature = "std"))]
mod test {
    use futures::executor::block_on;

    use crate::{
        blockdev::BlockDeviceWrite,
        layout::{
            DirectoryEntryTable, DiskRegion, VolumeDescriptor, SECTOR_SIZE_U64, VOLUME_HEADER_MAGIC,
        },
        read::{read_volume, VolumeError},
        write::fs::{
            MemoryFilesystem, SectorLinearBlockDevice, SectorLinearBlockFilesystem,
            SectorLinearImage,
        },
    };

    #[test]
    fn test_read_volume_from_disk_not_enough_disk_space() {
        let mut data = [0u8];
        let res = block_on(read_volume(data.as_mut_slice()));
        assert_eq!(res, Err(VolumeError::InvalidVolume));
    }

    #[test]
    fn test_read_volume_from_disk_invalid_volume() {
        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(MemoryFilesystem::default());

        let mut volume = [0u8; 0x800];
        volume[0..0x14].copy_from_slice(&VOLUME_HEADER_MAGIC);
        volume[0x7ec..0x800].copy_from_slice(&VOLUME_HEADER_MAGIC);
        volume[0x14] = 10;
        volume[0x18] = 20;

        // Invalidate header magic number
        volume[0] = 0;
        volume[0x7ec] = 0;

        block_on(slbd.write(32 * SECTOR_SIZE_U64, &volume)).expect("Write should succeed");

        let mut dev = SectorLinearImage::new(&slbd, &mut slbfs);
        let res = block_on(read_volume(&mut dev));
        assert_eq!(res, Err(VolumeError::InvalidVolume));
    }

    #[test]
    fn test_read_volume_from_disk_valid_volume() {
        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(MemoryFilesystem::default());

        let mut volume = [0u8; 0x800];
        volume[0..0x14].copy_from_slice(&VOLUME_HEADER_MAGIC);
        volume[0x7ec..0x800].copy_from_slice(&VOLUME_HEADER_MAGIC);
        volume[0x14] = 10;
        volume[0x18] = 20;

        block_on(slbd.write(32 * SECTOR_SIZE_U64, &volume)).expect("Write should succeed");

        let mut dev = SectorLinearImage::new(&slbd, &mut slbfs);
        let res = block_on(read_volume(&mut dev));
        let volume_expected = VolumeDescriptor::new(DirectoryEntryTable {
            region: DiskRegion {
                sector: 10,
                size: 20,
            },
        });
        assert_eq!(res, Ok(volume_expected));
    }
}
