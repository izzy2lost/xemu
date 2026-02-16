use maybe_async::maybe_async;

use crate::{blockdev::BlockDeviceRead, layout::DirectoryEntryNode, read::DirectoryEntryReadError};

impl DirectoryEntryNode {
    /// Read a DirectoryEntryNode from disk, including
    /// the name. Returns None if the node is not present.
    #[maybe_async]
    pub async fn read_from_disk<BDR: BlockDeviceRead + ?Sized>(
        dev: &mut BDR,
        offset: u64,
    ) -> Result<Option<Self>, DirectoryEntryReadError<BDR::ReadError>> {
        let mut dirent_buf = [0; 0xe];
        dev.read(offset, &mut dirent_buf)
            .await
            .map_err(DirectoryEntryReadError::IOError)?;

        let dirent = Self::deserialize(&dirent_buf, offset)
            .map_err(|_| DirectoryEntryReadError::DeserializationFailed)?;
        let Some(mut dirent) = dirent else {
            return Ok(None);
        };

        let name_len = dirent.node.dirent.filename_length as usize;
        let name_buf = &mut dirent.name[0..name_len];
        dev.read(offset + 0xe, name_buf)
            .await
            .map_err(DirectoryEntryReadError::IOError)?;

        Ok(Some(dirent))
    }
}

#[cfg(test)]
mod test {
    use futures::executor::block_on;

    use crate::layout::{
        DirectoryEntryDiskData, DirectoryEntryDiskNode, DirectoryEntryNode, DirentAttributes,
        DiskRegion,
    };

    #[test]
    fn test_read_dirent_node_from_disk_empty_ff_filled() {
        let mut data = [0xffu8; 0x10];
        let res = block_on(DirectoryEntryNode::read_from_disk(data.as_mut_slice(), 2));
        assert_eq!(res, Ok(None));
    }

    #[test]
    fn test_read_dirent_node_from_disk_empty_zero_filled() {
        let mut data = [0x00u8; 0x10];
        let res = block_on(DirectoryEntryNode::read_from_disk(data.as_mut_slice(), 2));
        assert_eq!(res, Ok(None));
    }

    #[test]
    fn test_read_dirent_node_from_disk_valid_dirent() {
        let data: &mut [u8] = &mut [
            0x2e, 0x2e, 0x2e, 0x2e, // padding for offset
            0x1, 0x1, 0x2, 0x2, 0x1, 0x0, 0x0, 0x0, 0x2, 0x0, 0x0, 0x0, 0xff, 0x2, 'A' as u8,
            'b' as u8,
        ];

        let res = block_on(DirectoryEntryNode::read_from_disk(data, 4))
            .expect("Read/deserialize should not fail");
        let mut dirent_name = [0u8; 256];
        dirent_name[0] = 'A' as u8;
        dirent_name[1] = 'b' as u8;

        assert_eq!(
            res,
            Some(DirectoryEntryNode {
                node: DirectoryEntryDiskNode {
                    left_entry_offset: 257,
                    right_entry_offset: 514,
                    dirent: DirectoryEntryDiskData {
                        data: DiskRegion { sector: 1, size: 2 },
                        attributes: DirentAttributes(255),
                        filename_length: 2,
                    },
                },
                name: dirent_name,
                offset: 4,
            })
        );
    }
}
