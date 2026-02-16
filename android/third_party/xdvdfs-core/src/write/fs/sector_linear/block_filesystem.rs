use core::convert::Infallible;

use crate::layout;
use crate::write::fs::{
    FileEntry, FilesystemCopier, FilesystemHierarchy, PathRef, SectorLinearBlockDevice,
    SectorLinearBlockRegion,
};
use alloc::vec::Vec;
use maybe_async::maybe_async;

#[cfg(not(feature = "sync"))]
use alloc::boxed::Box;

pub struct SectorLinearBlockFilesystem<F> {
    pub fs: F,
}

impl<F> SectorLinearBlockFilesystem<F>
where
    F: FilesystemHierarchy + FilesystemCopier<[u8]>,
{
    pub fn new(fs: F) -> Self {
        Self { fs }
    }
}

#[maybe_async]
impl<F> FilesystemHierarchy for SectorLinearBlockFilesystem<F>
where
    F: FilesystemHierarchy,
{
    type Error = <F as FilesystemHierarchy>::Error;

    async fn read_dir(&mut self, path: PathRef<'_>) -> Result<Vec<FileEntry>, Self::Error> {
        self.fs.read_dir(path).await
    }

    async fn clear_cache(&mut self) -> Result<(), Self::Error> {
        self.fs.clear_cache().await
    }
}

#[maybe_async]
impl<F> FilesystemCopier<SectorLinearBlockDevice> for SectorLinearBlockFilesystem<F>
where
    F: FilesystemCopier<[u8]>,
{
    type Error = Infallible;

    async fn copy_file_in(
        &mut self,
        src: PathRef<'_>,
        dest: &mut SectorLinearBlockDevice,
        input_offset: u64,
        output_offset: u64,
        size: u64,
    ) -> Result<u64, Self::Error> {
        assert_eq!(input_offset, 0);

        let sector = output_offset / layout::SECTOR_SIZE as u64;
        let output_offset = output_offset % layout::SECTOR_SIZE as u64;
        assert_eq!(output_offset, 0);

        let sector_span = size.div_ceil(layout::SECTOR_SIZE as u64);
        assert!(dest.check_sector_range_free(sector, sector_span));

        dest.contents
            .insert(
                sector,
                SectorLinearBlockRegion::File {
                    path: src.into(),
                    sectors: sector_span,
                },
            )
            .ok_or(())
            .expect_err("overwriting sectors is not implemented");

        Ok(size)
    }
}

#[cfg(test)]
mod test {
    use crate::write::fs::{
        FilesystemCopier, MemoryFilesystem, PathRef, SectorLinearBlockSectorContents,
    };

    use super::{SectorLinearBlockDevice, SectorLinearBlockFilesystem};

    #[test]
    fn test_sector_linear_fs_copier() {
        let memfs = MemoryFilesystem::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(memfs);
        let mut slbd = SectorLinearBlockDevice::default();

        let path: PathRef = "/a/b".into();

        futures::executor::block_on(async {
            assert_eq!(
                slbfs.copy_file_in(path, &mut slbd, 0, 2048, 5000,).await,
                Ok(5000)
            );
        });

        assert_eq!(slbd.get(0), SectorLinearBlockSectorContents::Fill(0));
        assert_eq!(slbd.get(1), SectorLinearBlockSectorContents::File(path));
        assert_eq!(slbd.get(2), SectorLinearBlockSectorContents::File(path));
        assert_eq!(slbd.get(3), SectorLinearBlockSectorContents::File(path));
        assert_eq!(slbd.get(4), SectorLinearBlockSectorContents::Fill(0));
    }
}
