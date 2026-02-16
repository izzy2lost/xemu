use super::fs::{PathCow, PathRef, PathVec};

#[allow(unused_variables)]
pub trait ProgressVisitor {
    fn directory_discovered(&mut self, num_entries: usize) {}

    fn entry_counts(&mut self, file_count: usize, dir_count: usize) {}

    fn directory_added(&mut self, path: PathRef<'_>, sector: u64) {}

    fn file_added(&mut self, path: PathRef<'_>, sector: u64) {}

    fn finished_copying_image_data(&mut self) {}

    fn finished(&mut self) {}
}

pub struct NoOpProgressVisitor;

impl ProgressVisitor for NoOpProgressVisitor {}

impl<T: FnMut(ProgressInfo<'_>)> ProgressVisitor for T {
    fn directory_discovered(&mut self, num_entries: usize) {
        (self)(ProgressInfo::DiscoveredDirectory(num_entries));
    }

    fn entry_counts(&mut self, file_count: usize, dir_count: usize) {
        (self)(ProgressInfo::FileCount(file_count));
        (self)(ProgressInfo::DirCount(dir_count));
    }

    fn directory_added(&mut self, path: PathRef<'_>, sector: u64) {
        (self)(ProgressInfo::DirAdded(path.into(), sector));
    }

    fn file_added(&mut self, path: PathRef<'_>, sector: u64) {
        (self)(ProgressInfo::FileAdded(path.into(), sector));
    }

    fn finished_copying_image_data(&mut self) {
        (self)(ProgressInfo::FinishedCopyingImageData);
    }

    fn finished(&mut self) {
        (self)(ProgressInfo::FinishedPacking);
    }
}

#[non_exhaustive]
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum ProgressInfo<'a> {
    DiscoveredDirectory(usize),
    FileCount(usize),
    DirCount(usize),
    DirAdded(PathCow<'a>, u64),
    FileAdded(PathCow<'a>, u64),
    FinishedCopyingImageData,
    FinishedPacking,
}

#[non_exhaustive]
#[derive(serde::Serialize, serde::Deserialize, Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum OwnedProgressInfo {
    DiscoveredDirectory(usize),
    FileCount(usize),
    DirCount(usize),
    DirAdded(PathVec, u64),
    FileAdded(PathVec, u64),
    FinishedCopyingImageData,
    FinishedPacking,
}

impl ProgressInfo<'_> {
    pub fn to_owned(self) -> OwnedProgressInfo {
        match self {
            Self::DiscoveredDirectory(len) => OwnedProgressInfo::DiscoveredDirectory(len),
            Self::FileCount(count) => OwnedProgressInfo::FileCount(count),
            Self::DirCount(count) => OwnedProgressInfo::DirCount(count),
            Self::DirAdded(path, size) => OwnedProgressInfo::DirAdded(path.to_owned(), size),
            Self::FileAdded(path, size) => OwnedProgressInfo::FileAdded(path.to_owned(), size),
            Self::FinishedCopyingImageData => OwnedProgressInfo::FinishedCopyingImageData,
            Self::FinishedPacking => OwnedProgressInfo::FinishedPacking,
        }
    }
}
