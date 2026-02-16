use alloc::{collections::BTreeMap, string::String};
use maybe_async::maybe_async;
use sha3::{Digest, Sha3_256};
use thiserror::Error;

use crate::{
    blockdev::BlockDeviceRead,
    layout::{DirectoryEntryNode, VolumeDescriptor},
    read::{DirectoryTableWalkError, DiskDataReadError},
};

#[non_exhaustive]
#[derive(Error, Debug, Copy, Clone, Eq, PartialEq)]
pub enum ChecksumError<E> {
    #[error("failed to read xdvdfs filesystem")]
    FilesystemError(#[from] DirectoryTableWalkError<E>),
    #[error("failed to read image contents")]
    ReadDataError(#[from] DiskDataReadError<E>),
}

#[maybe_async]
pub async fn checksum<BDR: BlockDeviceRead + ?Sized>(
    dev: &mut BDR,
    volume: &VolumeDescriptor,
) -> Result<[u8; 32], ChecksumError<BDR::ReadError>> {
    let mut hasher = Sha3_256::new();

    let tree = volume.root_table.file_tree(dev).await?;
    let mut iter: BTreeMap<String, DirectoryEntryNode> = BTreeMap::new();

    for (dir, file) in tree {
        let name = file
            .name_str()
            .map_err(Into::<DirectoryTableWalkError<_>>::into);
        let path = alloc::format!("{}/{}", dir, name?);
        iter.insert(path, file);
    }

    for (path, file) in iter {
        let is_dir = file.node.dirent.is_directory();

        hasher.update(path.as_bytes());

        if !is_dir {
            let data = file.node.dirent.read_data_all(dev).await?;
            hasher.update(data);
        }
    }

    let bytes = hasher.finalize();
    let output: [u8; 32] = bytes[..]
        .try_into()
        .expect("SHA256 output should be 32 bytes");
    Ok(output)
}

#[cfg(test)]
mod test {
    use futures::executor::block_on;

    use crate::{
        read::read_volume,
        write::{
            fs::{
                MemoryFilesystem, SectorLinearBlockDevice, SectorLinearBlockFilesystem,
                SectorLinearImage,
            },
            img::{create_xdvdfs_image, NoOpProgressVisitor},
        },
    };

    use super::checksum;

    #[test]
    fn test_checksum() {
        let mut fs = MemoryFilesystem::default();
        fs.create("/a/b/c", "Hello World\n".as_bytes());
        fs.create("/a/b/d", "Test data\n".as_bytes());
        fs.create("/a/e", "xdvdfs checksum\n".as_bytes());
        fs.create("/f", "datadatadata\n".as_bytes());

        let mut slbd = SectorLinearBlockDevice::default();
        let mut fs = SectorLinearBlockFilesystem::new(&mut fs);
        block_on(create_xdvdfs_image(&mut fs, &mut slbd, NoOpProgressVisitor))
            .expect("Image creation should succeed");

        let mut dev = SectorLinearImage::new(&slbd, &mut fs);

        let volume = block_on(read_volume(&mut dev)).expect("Volume should exist");
        let res = block_on(checksum(&mut dev, &volume))
            .expect("Checksum should be computed successfully");
        assert_eq!(
            res,
            [
                154, 255, 59, 107, 3, 163, 32, 25, 37, 133, 40, 250, 132, 208, 149, 99, 59, 239,
                96, 26, 138, 31, 215, 39, 201, 103, 229, 48, 28, 220, 196, 78
            ]
        );
    }
}
