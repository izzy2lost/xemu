use crate::layout::SECTOR_SIZE;

pub struct SectorAllocator {
    next_free: u32,
}

impl Default for SectorAllocator {
    fn default() -> Self {
        // FIXME: Allow files to fill sectors 0..=31
        Self { next_free: 33 }
    }
}

pub fn required_sectors(len: u64) -> u32 {
    if len != 0 {
        let sectors: u32 = (len / SECTOR_SIZE as u64)
            .try_into()
            .expect("number of sectors should fit in u32");
        sectors
            + if (len % SECTOR_SIZE as u64) as u32 > 0 {
                1
            } else {
                0
            }
    } else {
        // Entries must always occupy at least one sector
        1
    }
}

impl SectorAllocator {
    /// Allocates a contiguous set of sectors, big enough to fit `bytes`.
    /// Returns the number of the first sector in the allocation
    pub fn allocate_contiguous(&mut self, bytes: u64) -> u32 {
        let sectors = required_sectors(bytes);
        let allocation = self.next_free;
        self.next_free += sectors;
        allocation
    }
}

#[cfg(test)]
mod test {
    use super::{required_sectors, SectorAllocator};

    #[test]
    fn test_required_sectors_zero_len() {
        assert_eq!(required_sectors(0), 1);
    }

    #[test]
    fn test_required_sectors_aligned() {
        assert_eq!(required_sectors(4096), 2);
    }

    #[test]
    fn test_required_sectors_unaligned() {
        assert_eq!(required_sectors(8191), 4);
    }

    #[test]
    fn test_linear_sector_allocator_single() {
        let mut allocator = SectorAllocator::default();
        assert_eq!(allocator.allocate_contiguous(10), 33);
    }

    #[test]
    fn test_linear_sector_allocator_multiple() {
        let mut allocator = SectorAllocator::default();
        assert_eq!(allocator.allocate_contiguous(10), 33);
        assert_eq!(allocator.allocate_contiguous(4095), 34);
        assert_eq!(allocator.allocate_contiguous(2048), 36);
    }
}
