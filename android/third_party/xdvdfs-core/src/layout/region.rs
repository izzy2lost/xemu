use serde::{Deserialize, Serialize};
use thiserror::Error;

use super::SECTOR_SIZE_U64;

#[derive(Debug, Copy, Clone, Eq, PartialEq, Error)]
#[error("offset {offset} out of bounds for file of size {size}")]
pub struct OutOfBounds {
    pub offset: u64,
    pub size: u32,
}

/// Represents a contiguous region on the disk image, given by sector number and
/// size.
#[repr(C)]
#[repr(packed)]
#[derive(Deserialize, Serialize, Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub struct DiskRegion {
    pub sector: u32,
    pub size: u32,
}

impl DiskRegion {
    pub fn is_empty(&self) -> bool {
        self.size == 0
    }

    pub fn offset(&self, offset: u64) -> Result<u64, OutOfBounds> {
        if offset >= self.size as u64 {
            return Err(OutOfBounds {
                offset,
                size: self.size,
            });
        }

        let offset = SECTOR_SIZE_U64 * self.sector as u64 + offset;
        Ok(offset)
    }
}

#[cfg(test)]
mod test {
    use crate::layout::OutOfBounds;

    use super::DiskRegion;

    #[test]
    fn test_layout_region_empty() {
        let region = DiskRegion {
            sector: 10,
            size: 0,
        };

        assert_eq!({ region.size }, 0);
        assert!(region.is_empty());
    }

    #[test]
    fn test_layout_region_non_empty() {
        let region = DiskRegion {
            sector: 10,
            size: 10,
        };

        assert_eq!({ region.size }, 10);
        assert!(!region.is_empty());
    }

    #[test]
    fn test_layout_region_offset_out_of_bounds() {
        let region = DiskRegion { sector: 3, size: 7 };

        let res = region.offset(11);
        assert_eq!(
            res,
            Err(OutOfBounds {
                offset: 11,
                size: 7
            })
        );
    }

    #[test]
    fn test_layout_region_offset_in_bounds() {
        let region = DiskRegion { sector: 3, size: 7 };

        let res = region.offset(5);
        assert_eq!(res, Ok(6149));
    }
}
