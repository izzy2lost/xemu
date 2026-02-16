use crate::layout::{DirentName, NameEncodingError};

use super::{DirectoryEntryDiskData, DirentAttributes, DiskRegion};

/// In-memory structure to contain the on-disk dirent data,
/// and file name information.
///
/// This does not contain information about on-disk left or
/// right subtrees.
///
/// Intended use is for building the dirent tree within some other
/// data structure, and then creating the on-disk structure separately
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct DirectoryEntryData<'alloc> {
    pub node: DirectoryEntryDiskData,
    name: DirentName<'alloc>,
    pub idx: usize,
}

impl<'alloc> DirectoryEntryData<'alloc> {
    pub fn new_without_sector(
        name: &'alloc str,
        size: u32,
        attributes: DirentAttributes,
        idx: usize,
    ) -> Result<Self, NameEncodingError> {
        if name.len() > 255 {
            return Err(NameEncodingError::NameTooLong);
        }

        let filename_length = name
            .len()
            .try_into()
            .map_err(|_| NameEncodingError::NameTooLong)?;
        let name = DirentName::new(name);

        Ok(Self {
            node: DirectoryEntryDiskData {
                data: DiskRegion { sector: 0, size },
                attributes,
                filename_length,
            },
            name,
            idx,
        })
    }

    pub fn get_name(&self) -> &str {
        self.name.get_name()
    }

    pub fn compute_len_and_name_encoding(
        &mut self,
    ) -> Result<u8, crate::write::FileStructureError> {
        Ok(self.name.set_encode_name()?)
    }

    pub fn get_encoded_name(&self) -> &[u8] {
        self.name.get_encoded_name()
    }

    /// Returns the length (in bytes) of the directory entry
    /// on disk, after serialization
    pub fn len_on_disk(&self) -> u32 {
        let encoded_filename_len = self.get_encoded_name().len() as u32;
        let size = 0xe + encoded_filename_len;

        size.next_multiple_of(4)
    }
}

impl PartialOrd for DirectoryEntryData<'_> {
    fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for DirectoryEntryData<'_> {
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        self.name.cmp(&other.name)
    }
}

#[cfg(test)]
mod test {
    use crate::layout::DirentAttributes;

    use super::DirectoryEntryData;

    #[test]
    fn test_layout_dirent_data_cmp() {
        let d1 = DirectoryEntryData::new_without_sector("abc", 10, DirentAttributes(0xff), 0)
            .expect("Dirent is valid");
        let d2 = DirectoryEntryData::new_without_sector("ABC", 52, DirentAttributes(0x00), 1)
            .expect("Dirent is valid");

        assert_eq!(d1.partial_cmp(&d2), Some(core::cmp::Ordering::Equal));
    }
}
