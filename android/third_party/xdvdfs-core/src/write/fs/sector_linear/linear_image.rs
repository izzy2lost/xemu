use core::ops::{Deref, DerefMut};

use crate::blockdev::{BlockDeviceRead, BlockDeviceWrite};
use crate::layout;
use crate::write::fs::{FilesystemCopier, SectorLinearBlockDevice, SectorLinearBlockFilesystem};
use alloc::{vec, vec::Vec};
use maybe_async::maybe_async;

#[cfg(not(feature = "sync"))]
use alloc::boxed::Box;

use super::SectorLinearBlockRegion;

pub struct SectorLinearImage<SLBD, SLBFS> {
    linear: SLBD,
    fs: SLBFS,
}

impl<F, SLBD, SLBFS> SectorLinearImage<SLBD, SLBFS>
where
    SLBD: Deref<Target = SectorLinearBlockDevice>,
    SLBFS: DerefMut<Target = SectorLinearBlockFilesystem<F>>,
{
    pub fn new(bdev: SLBD, fs: SLBFS) -> Self {
        Self { linear: bdev, fs }
    }
}

impl<F, FSE, SLBD, SLBFS> SectorLinearImage<SLBD, SLBFS>
where
    F: FilesystemCopier<[u8], Error = FSE>,
    SLBD: Deref<Target = SectorLinearBlockDevice>,
    SLBFS: DerefMut<Target = SectorLinearBlockFilesystem<F>>,
{
    #[maybe_async]
    pub async fn read_linear(&mut self, offset: u64, size: u64) -> Result<Vec<u8>, FSE> {
        let mut sector = offset / (layout::SECTOR_SIZE as u64);
        let mut position = offset % (layout::SECTOR_SIZE as u64);

        let size = size as usize;
        let mut buffer = vec![0; size];
        let mut index: usize = 0;

        let mut iter = self.linear.sector_range(sector..);

        while index < size {
            let Some((incoming_sector, contents)) = iter.next() else {
                // Out of sectors, truncate buffer to actual size
                buffer.resize(index, 0);
                break;
            };

            if incoming_sector > sector {
                let sector_gap = incoming_sector - sector;
                let empty_len = core::cmp::min(
                    (size - index) as u64,
                    sector_gap * layout::SECTOR_SIZE as u64 - position,
                );
                index += empty_len as usize;
                position = 0;
                sector += sector_gap;

                if index >= size {
                    break;
                }
            }

            let remaining = size - index;
            let content_offset = sector.saturating_sub(incoming_sector);
            let content_offset_sector = content_offset * layout::SECTOR_SIZE as u64;
            let to_read = core::cmp::min(
                remaining as u64,
                contents.size_bytes() - content_offset_sector - position,
            ) as usize;

            match contents {
                SectorLinearBlockRegion::RawData(data) => {
                    let data = &data[(content_offset_sector as usize)..];
                    let position = position as usize;
                    let end = position + to_read;
                    buffer[index..(index + to_read)].clone_from_slice(&data[position..end]);
                }
                SectorLinearBlockRegion::File { path, .. } => {
                    self.fs
                        .fs
                        .copy_file_in(
                            path.into(),
                            &mut buffer[index..(index + to_read)],
                            position + content_offset * layout::SECTOR_SIZE as u64,
                            0,
                            to_read as u64,
                        )
                        .await?;
                }
                SectorLinearBlockRegion::Fill { byte, .. } if *byte != 0 => {
                    buffer[index..(index + to_read)].fill(*byte);
                }
                SectorLinearBlockRegion::Fill { .. } => {}
            }

            index += to_read;
            position = 0;
            sector += contents.size_sectors() - content_offset;
        }

        Ok(buffer)
    }
}

#[cfg(feature = "ciso_support")]
#[maybe_async]
impl<F, SLBD, SLBFS> ciso::write::SectorReader for SectorLinearImage<SLBD, SLBFS>
where
    F: FilesystemCopier<[u8]>,
    SLBD: Deref<Target = SectorLinearBlockDevice> + Send + Sync,
    SLBFS: DerefMut<Target = SectorLinearBlockFilesystem<F>> + Send + Sync,
{
    type ReadError = F::Error;

    async fn size(&mut self) -> Result<u64, Self::ReadError> {
        Ok(self.linear.size())
    }

    async fn read_sector(
        &mut self,
        sector: usize,
        sector_size: u32,
    ) -> Result<Vec<u8>, Self::ReadError> {
        let offset = sector as u64 * sector_size as u64;
        let mut data = self.read_linear(offset, sector_size as u64).await?;
        if data.len() < sector_size as usize {
            data.resize(sector_size as usize, 0);
        }

        Ok(data)
    }
}

#[maybe_async]
impl<F, SLBD, SLBFS> BlockDeviceRead for SectorLinearImage<SLBD, SLBFS>
where
    F: FilesystemCopier<[u8]>,
    SLBD: Deref<Target = SectorLinearBlockDevice> + Send + Sync,
    SLBFS: DerefMut<Target = SectorLinearBlockFilesystem<F>> + Send + Sync,
{
    type ReadError = F::Error;

    async fn read(&mut self, offset: u64, buffer: &mut [u8]) -> Result<(), Self::ReadError> {
        let len = <[u8]>::len(buffer);
        let mut data = self.read_linear(offset, len as u64).await?;
        data.resize(len, 0);
        buffer.copy_from_slice(&data);
        Ok(())
    }
}

#[maybe_async]
impl<F, FSE, SLBD, SLBFS> BlockDeviceWrite for SectorLinearImage<SLBD, SLBFS>
where
    F: FilesystemCopier<[u8], Error = FSE>,
    SLBD: DerefMut<Target = SectorLinearBlockDevice> + Send + Sync,
    SLBFS: DerefMut<Target = SectorLinearBlockFilesystem<F>> + Send + Sync,
{
    type WriteError = core::convert::Infallible;

    async fn write(&mut self, offset: u64, buffer: &[u8]) -> Result<(), Self::WriteError> {
        self.linear.write(offset, buffer).await
    }

    async fn len(&mut self) -> Result<u64, Self::WriteError> {
        self.linear.len().await
    }
}

#[cfg(test)]
mod test {
    use crate::write::fs::{MemoryFilesystem, PathVec, SectorLinearBlockRegion};

    use super::{SectorLinearBlockDevice, SectorLinearBlockFilesystem, SectorLinearImage};

    #[test]
    fn test_linear_image_read_zero_fill_entry() {
        let memfs = MemoryFilesystem::default();
        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(memfs);

        slbd.contents.insert(
            5,
            SectorLinearBlockRegion::Fill {
                byte: 0,
                sectors: 2,
            },
        );

        let mut image = SectorLinearImage::new(&slbd, &mut slbfs);
        futures::executor::block_on(async {
            let data = image
                .read_linear(5 * 2048, 2048)
                .await
                .expect("Read should return data");
            assert_eq!(data.len(), 2048);
            assert_eq!(data, alloc::vec![0; 2048]);
        });
    }

    #[test]
    fn test_linear_image_read_nonzero_fill_entry() {
        let memfs = MemoryFilesystem::default();
        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(memfs);

        slbd.contents.insert(
            5,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );

        let mut image = SectorLinearImage::new(&slbd, &mut slbfs);
        futures::executor::block_on(async {
            let data = image
                .read_linear(5 * 2048, 2048)
                .await
                .expect("Read should return data");
            assert_eq!(data.len(), 2048);
            assert_eq!(data, alloc::vec![0xff; 2048]);
        });
    }

    #[test]
    fn test_linear_image_read_empty_image() {
        let memfs = MemoryFilesystem::default();
        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(memfs);

        // Insert sector to give image size
        slbd.contents.insert(
            5,
            SectorLinearBlockRegion::Fill {
                byte: 0,
                sectors: 2,
            },
        );

        let mut image = SectorLinearImage::new(&slbd, &mut slbfs);
        futures::executor::block_on(async {
            let data = image
                .read_linear(0, 2048)
                .await
                .expect("Read should return data");
            assert_eq!(data.len(), 2048);
            assert_eq!(data, alloc::vec![0; 2048]);
        });
    }

    #[test]
    fn test_linear_image_eof_truncated_output() {
        let memfs = MemoryFilesystem::default();
        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(memfs);

        // Insert sector to give image size
        slbd.contents.insert(
            0,
            SectorLinearBlockRegion::Fill {
                byte: 0,
                sectors: 1,
            },
        );

        let mut image = SectorLinearImage::new(&slbd, &mut slbfs);
        futures::executor::block_on(async {
            let data = image
                .read_linear(0, 4096)
                .await
                .expect("Read should return data");
            assert_eq!(data.len(), 2048);
            assert_eq!(data, alloc::vec![0; 2048]);
        });
    }

    #[test]
    fn test_linear_image_read_data() {
        let memfs = MemoryFilesystem::default();
        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(memfs);

        let data = alloc::vec![10; 2048];
        slbd.contents
            .insert(0, SectorLinearBlockRegion::RawData(data.into_boxed_slice()));

        let mut image = SectorLinearImage::new(&slbd, &mut slbfs);
        futures::executor::block_on(async {
            let data = image
                .read_linear(0, 2048)
                .await
                .expect("Read should return data");
            assert_eq!(data.len(), 2048);
            assert_eq!(data, alloc::vec![10; 2048]);
        });
    }

    #[test]
    fn test_linear_image_read_data_offset() {
        let memfs = MemoryFilesystem::default();
        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(memfs);

        let data = alloc::vec![10; 2048];
        slbd.contents
            .insert(0, SectorLinearBlockRegion::RawData(data.into_boxed_slice()));
        slbd.contents.insert(
            1,
            SectorLinearBlockRegion::Fill {
                byte: 0,
                sectors: 1,
            },
        );

        let mut image = SectorLinearImage::new(&slbd, &mut slbfs);
        futures::executor::block_on(async {
            let data = image
                .read_linear(1024, 2048)
                .await
                .expect("Read should return data");
            assert_eq!(data.len(), 2048);
            assert_eq!(data[0..1024], alloc::vec![10; 1024]);
            assert_eq!(data[1024..], alloc::vec![0; 1024]);
        });
    }

    #[test]
    fn test_linear_image_read_data_sized() {
        let memfs = MemoryFilesystem::default();
        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(memfs);

        let data = alloc::vec![10; 2048];
        slbd.contents
            .insert(0, SectorLinearBlockRegion::RawData(data.into_boxed_slice()));

        let mut image = SectorLinearImage::new(&slbd, &mut slbfs);
        futures::executor::block_on(async {
            let data = image
                .read_linear(0, 1024)
                .await
                .expect("Read should return data");
            assert_eq!(data.len(), 1024);
            assert_eq!(data[0..1024], alloc::vec![10; 1024]);
        });
    }

    #[test]
    fn test_linear_image_read_file() {
        let mut memfs = MemoryFilesystem::default();
        memfs.create("/a/b", &alloc::vec![10; 4000]);

        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(memfs);

        let path = PathVec::from("/a/b");
        slbd.contents
            .insert(0, SectorLinearBlockRegion::File { path, sectors: 2 });

        let mut image = SectorLinearImage::new(&slbd, &mut slbfs);
        futures::executor::block_on(async {
            let data = image
                .read_linear(0, 4096)
                .await
                .expect("Read should return data");
            assert_eq!(data.len(), 4096);
            assert_eq!(data[0..4000], alloc::vec![10; 4000]);
            assert_eq!(data[4000..], alloc::vec![0; 96]);
        });
    }

    #[test]
    fn test_linear_image_read_file_offset() {
        let mut memfs = MemoryFilesystem::default();
        memfs.create("/a/b", &alloc::vec![10; 4000]);

        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(memfs);

        let path = PathVec::from("/a/b");
        slbd.contents
            .insert(0, SectorLinearBlockRegion::File { path, sectors: 2 });

        let mut image = SectorLinearImage::new(&slbd, &mut slbfs);
        futures::executor::block_on(async {
            let data = image
                .read_linear(1024, 4096)
                .await
                .expect("Read should return data");
            assert_eq!(data.len(), 3072);

            // File size of 4000 - offset of 1024
            assert_eq!(data[0..2976], alloc::vec![10; 2976]);
            assert_eq!(data[2976..], alloc::vec![0; 96]);
        });
    }

    #[test]
    fn test_linear_image_read_file_sized() {
        let mut memfs = MemoryFilesystem::default();
        memfs.create("/a/b", &alloc::vec![10; 4000]);

        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(memfs);

        let path = PathVec::from("/a/b");
        slbd.contents
            .insert(0, SectorLinearBlockRegion::File { path, sectors: 2 });

        let mut image = SectorLinearImage::new(&slbd, &mut slbfs);
        futures::executor::block_on(async {
            let data = image
                .read_linear(0, 3072)
                .await
                .expect("Read should return data");
            assert_eq!(data.len(), 3072);
            assert_eq!(data[0..3072], alloc::vec![10; 3072]);
        });
    }

    #[test]
    fn test_linear_image_read_sparse_multi_sector() {
        let mut memfs = MemoryFilesystem::default();
        memfs.create("/a/b", &alloc::vec![10; 2000]);

        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(memfs);

        slbd.contents.insert(
            3,
            SectorLinearBlockRegion::File {
                path: "/a/b".into(),
                sectors: 1,
            },
        );
        let data = alloc::vec![15; 2048];
        slbd.contents
            .insert(5, SectorLinearBlockRegion::RawData(data.into_boxed_slice()));

        let mut image = SectorLinearImage::new(&slbd, &mut slbfs);
        futures::executor::block_on(async {
            // Size: 1024 from sector 1, then sectors 2, 3, 4, 5
            let data = image
                .read_linear(3072, 2048 * 4 + 1024)
                .await
                .expect("Read should return data");
            assert_eq!(data.len(), 2048 * 4 + 1024);

            // Sector 1 and 2 are empty
            assert_eq!(data[0..3072], alloc::vec![0; 3072]);
            // Sector 3 is a file, last 48 bytes are zero
            assert_eq!(data[3072..5072], alloc::vec![10; 2000]);
            assert_eq!(data[5072..5120], alloc::vec![0; 48]);
            // Sector 4 is empty
            assert_eq!(data[5120..7168], alloc::vec![0; 2048]);
            // Sector 5 is data
            assert_eq!(data[7168..], alloc::vec![15; 2048]);
        })
    }
}
