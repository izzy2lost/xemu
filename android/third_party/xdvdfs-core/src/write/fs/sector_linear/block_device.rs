use core::{convert::Infallible, ops::RangeBounds};

use crate::{
    blockdev::BlockDeviceWrite,
    layout,
    write::fs::{PathRef, PathVec},
};
use alloc::boxed::Box;
use maybe_async::maybe_async;

/// Contents for a region of the sector linear block device
/// The sector offset is specified as the key in the contents tree,
/// and the sector lengt
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum SectorLinearBlockRegion {
    // Slice of data filling the region
    // Its length should be a multiple of sector size
    RawData(Box<[u8]>),
    File { path: PathVec, sectors: u64 },
    Fill { byte: u8, sectors: u64 },
}

/// Contents of a single sector within the sector linear block device
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SectorLinearBlockSectorContents<'a> {
    RawData(&'a [u8]),
    File(PathRef<'a>),
    Fill(u8),
}

impl SectorLinearBlockRegion {
    pub fn size_sectors(&self) -> u64 {
        let used_sectors = match self {
            Self::RawData(data) => {
                let len = <[u8]>::len(data);
                assert_eq!(len % (layout::SECTOR_SIZE as usize), 0);
                len.div_ceil(layout::SECTOR_SIZE as usize) as u64
            }
            Self::File { sectors, .. } => *sectors,
            Self::Fill { sectors, .. } => *sectors,
        };

        // An entry must occupy at least the sector it exists at,
        // even if it fills it with no data
        core::cmp::max(used_sectors, 1)
    }

    pub fn size_bytes(&self) -> u64 {
        self.size_sectors() * layout::SECTOR_SIZE as u64
    }

    pub fn into_contents(&self, sector_offset: u64) -> SectorLinearBlockSectorContents<'_> {
        assert!(sector_offset < self.size_sectors());
        match self {
            Self::RawData(data) => {
                let sector_size = layout::SECTOR_SIZE as usize;
                let start = (sector_offset as usize) * sector_size;
                let end = start + sector_size;
                SectorLinearBlockSectorContents::RawData(&data[start..end])
            }
            Self::File { path, .. } => SectorLinearBlockSectorContents::File(path.into()),
            Self::Fill { byte, .. } => SectorLinearBlockSectorContents::Fill(*byte),
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct SectorLinearBlockDevice {
    pub(super) contents: alloc::collections::BTreeMap<u64, SectorLinearBlockRegion>,
}

impl SectorLinearBlockDevice {
    pub fn num_sectors(&self) -> u64 {
        self.contents
            .last_key_value()
            .map(|(sector, contents)| *sector + contents.size_sectors())
            .unwrap_or(0)
    }

    pub fn size(&self) -> u64 {
        self.num_sectors() * (layout::SECTOR_SIZE as u64)
    }

    pub fn clear(&mut self) {
        self.contents.clear();
    }

    fn get_or_empty(&self, sector: u64) -> Option<(u64, &SectorLinearBlockRegion)> {
        let index = sector;
        self.contents
            .range(..=index)
            .next_back()
            .filter(|(sector, data)| index >= **sector && index < **sector + data.size_sectors())
            .map(|(sector, data)| (*sector, data))
    }

    pub fn get(&self, sector: u64) -> SectorLinearBlockSectorContents<'_> {
        let index = sector;
        self.get_or_empty(index)
            .inspect(|(sector, _)| assert!(index >= *sector))
            .map(|(sector, data)| data.into_contents(index - sector))
            .unwrap_or(SectorLinearBlockSectorContents::Fill(0))
    }

    pub fn sector_range<R: RangeBounds<u64>>(
        &self,
        range: R,
    ) -> impl Iterator<Item = (u64, &SectorLinearBlockRegion)> {
        use core::ops::Bound;
        let start_incl_bound = match range.start_bound() {
            Bound::Included(bound) => Some(*bound),
            Bound::Excluded(bound) => Some(*bound + 1),
            Bound::Unbounded => None,
        };

        let range_iter = self
            .contents
            .range(range)
            .map(|(sector, data)| (*sector, data));

        // Include data overlapping the start bound, if applicable.
        // Exclude if the sector is the same as the first bound sector,
        // as it will be included in the range_iter already.
        let mut start_incl_data = start_incl_bound
            .and_then(|bound| self.get_or_empty(bound))
            .filter(|(sector, _)| Some(*sector) != start_incl_bound);
        core::iter::from_fn(move || start_incl_data.take()).chain(range_iter)
    }

    pub fn check_sector_range_free(&self, sector: u64, num_sectors: u64) -> bool {
        // Range is free as long as the end of the previous sector interval
        // does not cross the start of the new interval.
        self.contents
            .range(..(sector + num_sectors))
            .next_back()
            .is_none_or(|ent| *ent.0 + ent.1.size_sectors() <= sector)
    }
}

#[maybe_async]
impl BlockDeviceWrite for SectorLinearBlockDevice {
    type WriteError = Infallible;

    async fn write(&mut self, offset: u64, buffer: &[u8]) -> Result<(), Infallible> {
        let sector = offset / layout::SECTOR_SIZE as u64;

        let offset = offset % layout::SECTOR_SIZE as u64;
        assert_eq!(offset, 0);

        assert!(self.check_sector_range_free(
            sector,
            buffer.len().div_ceil(layout::SECTOR_SIZE as usize) as u64
        ));

        let mut data = buffer.to_vec();
        data.resize(
            buffer.len().next_multiple_of(layout::SECTOR_SIZE as usize),
            0,
        );
        self.contents
            .insert(
                sector,
                SectorLinearBlockRegion::RawData(data.into_boxed_slice()),
            )
            .ok_or(())
            .expect_err("overwriting sectors is not implemented");

        Ok(())
    }

    async fn len(&mut self) -> Result<u64, Infallible> {
        Ok(self.size())
    }
}

#[cfg(test)]
mod test {
    use core::ops::Bound;

    use crate::write::fs::{PathVec, SectorLinearBlockRegion};

    use super::{SectorLinearBlockDevice, SectorLinearBlockSectorContents};

    #[test]
    fn test_sector_linear_dev_clear() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            5,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.clear();
        assert_eq!(slbd.size(), 0);
    }

    #[test]
    fn test_sector_linear_dev_size_end_fill() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            5,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        assert_eq!(slbd.size(), 7 * 2048);
    }

    #[test]
    fn test_sector_linear_dev_size_end_raw_data() {
        let mut slbd = SectorLinearBlockDevice::default();
        let data = alloc::vec![0; 4096].into_boxed_slice();
        slbd.contents
            .insert(5, SectorLinearBlockRegion::RawData(data));
        assert_eq!(slbd.size(), 7 * 2048);
    }

    #[test]
    fn test_sector_linear_dev_size_end_file() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            5,
            SectorLinearBlockRegion::File {
                path: PathVec::default(),
                sectors: 3,
            },
        );
        assert_eq!(slbd.size(), 8 * 2048);
    }

    #[test]
    fn test_sector_linear_dev_num_sectors_fill() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            5,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 1,
            },
        );
        assert_eq!(slbd.num_sectors(), 6);
    }

    #[test]
    fn test_sector_linear_dev_num_sectors_file() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            5,
            SectorLinearBlockRegion::File {
                path: PathVec::default(),
                sectors: 1,
            },
        );
        assert_eq!(slbd.num_sectors(), 6);
    }

    #[test]
    fn test_sector_linear_dev_num_sectors_data() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            5,
            SectorLinearBlockRegion::RawData(alloc::vec![0; 2048].into_boxed_slice()),
        );
        assert_eq!(slbd.num_sectors(), 6);
    }

    #[test]
    fn test_sector_linear_dev_num_sectors_zero_length_fill() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            5,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 0,
            },
        );
        assert_eq!(slbd.num_sectors(), 6);
    }

    #[test]
    fn test_sector_linear_dev_num_sectors_zero_length_file() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            5,
            SectorLinearBlockRegion::File {
                path: PathVec::default(),
                sectors: 0,
            },
        );
        assert_eq!(slbd.num_sectors(), 6);
    }

    #[test]
    fn test_sector_linear_dev_index_empty() {
        let slbd = SectorLinearBlockDevice::default();
        assert_eq!(slbd.get(0), SectorLinearBlockSectorContents::Fill(0));
    }

    #[test]
    fn test_sector_linear_dev_index_fill() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            5,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        assert_eq!(slbd.get(5), SectorLinearBlockSectorContents::Fill(0xff));
    }

    #[test]
    fn test_sector_linear_dev_write_aligned() {
        use crate::blockdev::BlockDeviceWrite;

        let mut slbd = SectorLinearBlockDevice::default();
        futures::executor::block_on(async {
            slbd.write(2048, &[1, 2, 3, 4, 5])
                .await
                .expect("write should succeed");
        });

        assert_eq!(slbd.get(0), SectorLinearBlockSectorContents::Fill(0));
        let SectorLinearBlockSectorContents::RawData(data) = slbd.get(1) else {
            panic!("Sector 1 should contain raw data");
        };
        assert_eq!(data[0..5], [1, 2, 3, 4, 5]);
        assert_eq!(slbd.get(2), SectorLinearBlockSectorContents::Fill(0));
    }

    #[test]
    fn test_sector_linear_dev_write_multi_sector() {
        use crate::blockdev::BlockDeviceWrite;

        let mut slbd = SectorLinearBlockDevice::default();
        let data = alloc::vec![10; 2048 * 2];
        futures::executor::block_on(async {
            slbd.write(2048, &data).await.expect("write should succeed");
        });

        assert_eq!(slbd.get(0), SectorLinearBlockSectorContents::Fill(0));
        let SectorLinearBlockSectorContents::RawData(data) = slbd.get(1) else {
            panic!("Sector 1 should contain raw data");
        };
        assert_eq!(data[0..5], [10, 10, 10, 10, 10]);

        let SectorLinearBlockSectorContents::RawData(data) = slbd.get(2) else {
            panic!("Sector 2 should contain raw data");
        };
        assert_eq!(data[0..5], [10, 10, 10, 10, 10]);
        assert_eq!(slbd.get(3), SectorLinearBlockSectorContents::Fill(0));
    }

    #[test]
    fn test_sector_linear_dev_write_non_sequential() {
        use crate::blockdev::BlockDeviceWrite;

        let mut slbd = SectorLinearBlockDevice::default();
        let data = alloc::vec![10; 2048];
        futures::executor::block_on(async {
            slbd.write(4096, &data).await.expect("write should succeed");
            slbd.write(2048, &data).await.expect("write should succeed");
        });

        assert_eq!(slbd.get(0), SectorLinearBlockSectorContents::Fill(0));
        let SectorLinearBlockSectorContents::RawData(data) = slbd.get(1) else {
            panic!("Sector 1 should contain raw data");
        };
        assert_eq!(data[0..5], [10, 10, 10, 10, 10]);

        let SectorLinearBlockSectorContents::RawData(data) = slbd.get(2) else {
            panic!("Sector 2 should contain raw data");
        };
        assert_eq!(data[0..5], [10, 10, 10, 10, 10]);
        assert_eq!(slbd.get(3), SectorLinearBlockSectorContents::Fill(0));
    }

    #[test]
    fn test_sector_linear_range_iterator_start_not_contained() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            4,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 1,
            },
        );
        slbd.contents.insert(
            6,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            8,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 1,
            },
        );
        slbd.contents.insert(
            9,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );

        let result: alloc::vec::Vec<(u64, &SectorLinearBlockRegion)> =
            slbd.sector_range(5..=9).collect();
        assert_eq!(
            result,
            &[
                (
                    6,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
                (
                    8,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 1
                    }
                ),
                (
                    9,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
            ]
        );
    }

    #[test]
    fn test_sector_linear_range_iterator_start_contained_by_prev() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            4,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            6,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            8,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 1,
            },
        );
        slbd.contents.insert(
            9,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );

        let result: alloc::vec::Vec<(u64, &SectorLinearBlockRegion)> =
            slbd.sector_range(5..=9).collect();
        assert_eq!(
            result,
            &[
                (
                    4,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
                (
                    6,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
                (
                    8,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 1
                    }
                ),
                (
                    9,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
            ]
        );
    }

    #[test]
    fn test_sector_linear_range_iterator_unbounded_below() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            2,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            4,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            6,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            8,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 1,
            },
        );
        slbd.contents.insert(
            9,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );

        let result: alloc::vec::Vec<(u64, &SectorLinearBlockRegion)> =
            slbd.sector_range(..9).collect();
        assert_eq!(
            result,
            &[
                (
                    2,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
                (
                    4,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
                (
                    6,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
                (
                    8,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 1
                    }
                ),
            ]
        );
    }

    #[test]
    fn test_sector_linear_range_iterator_unbounded_above() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            2,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            4,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            6,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            8,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 1,
            },
        );
        slbd.contents.insert(
            9,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );

        let result: alloc::vec::Vec<(u64, &SectorLinearBlockRegion)> =
            slbd.sector_range(5..).collect();
        assert_eq!(
            result,
            &[
                (
                    4,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
                (
                    6,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
                (
                    8,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 1
                    }
                ),
                (
                    9,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
            ]
        );
    }

    #[test]
    fn test_sector_linear_range_iterator_equal() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            2,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            4,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            6,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            8,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 1,
            },
        );
        slbd.contents.insert(
            9,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );

        let result: alloc::vec::Vec<(u64, &SectorLinearBlockRegion)> =
            slbd.sector_range(4..).collect();
        assert_eq!(
            result,
            &[
                (
                    4,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
                (
                    6,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
                (
                    8,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 1
                    }
                ),
                (
                    9,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
            ]
        );
    }

    #[test]
    fn test_sector_linear_range_iterator_excluded_start() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            4,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            6,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            8,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 1,
            },
        );
        slbd.contents.insert(
            9,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );

        let result: alloc::vec::Vec<(u64, &SectorLinearBlockRegion)> = slbd
            .sector_range((Bound::Excluded(5), Bound::Included(9)))
            .collect();
        assert_eq!(
            result,
            &[
                (
                    6,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
                (
                    8,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 1
                    }
                ),
                (
                    9,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
            ]
        );
    }

    #[test]
    fn test_sector_linear_range_iterator_excluded_start_included_sector() {
        let mut slbd = SectorLinearBlockDevice::default();
        slbd.contents.insert(
            4,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            6,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );
        slbd.contents.insert(
            8,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 1,
            },
        );
        slbd.contents.insert(
            9,
            SectorLinearBlockRegion::Fill {
                byte: 0xff,
                sectors: 2,
            },
        );

        let result: alloc::vec::Vec<(u64, &SectorLinearBlockRegion)> = slbd
            .sector_range((Bound::Excluded(4), Bound::Included(9)))
            .collect();
        assert_eq!(
            result,
            &[
                (
                    4,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
                (
                    6,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
                (
                    8,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 1
                    }
                ),
                (
                    9,
                    &SectorLinearBlockRegion::Fill {
                        byte: 0xff,
                        sectors: 2
                    }
                ),
            ]
        );
    }
}
