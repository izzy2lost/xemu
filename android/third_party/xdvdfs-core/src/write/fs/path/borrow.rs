use core::fmt::Display;

use super::{PathRef, PathVec};

#[derive(Debug, Clone)]
pub enum PathCow<'a> {
    Borrowed(PathRef<'a>),
    Owned(PathVec),
}

impl<'a> From<PathVec> for PathCow<'a> {
    fn from(value: PathVec) -> Self {
        Self::Owned(value)
    }
}

impl<'a> From<PathRef<'a>> for PathCow<'a> {
    fn from(value: PathRef<'a>) -> Self {
        Self::Borrowed(value)
    }
}

impl<'a> PathCow<'a> {
    pub fn path_ref(&self) -> PathRef<'_> {
        match self {
            Self::Borrowed(pr) => *pr,
            Self::Owned(pvr) => pvr.as_path_ref(),
        }
    }

    pub fn to_owned(self) -> PathVec {
        match self {
            Self::Borrowed(pr) => pr.into(),
            Self::Owned(pvr) => pvr,
        }
    }
}

impl<'a> PartialEq<PathCow<'a>> for PathCow<'a> {
    fn eq(&self, other: &PathCow<'a>) -> bool {
        self.path_ref() == other.path_ref()
    }
}

impl<'a> PartialEq<PathRef<'a>> for PathCow<'a> {
    fn eq(&self, other: &PathRef<'a>) -> bool {
        self.path_ref() == *other
    }
}

impl<'a> PartialEq<PathVec> for PathCow<'a> {
    fn eq(&self, other: &PathVec) -> bool {
        self.path_ref() == other.as_path_ref()
    }
}

impl<'a> Eq for PathCow<'a> {}

impl<'a> Ord for PathCow<'a> {
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        self.path_ref().cmp(&other.path_ref())
    }
}

impl<'a> PartialOrd<PathCow<'a>> for PathCow<'a> {
    fn partial_cmp(&self, other: &PathCow<'a>) -> Option<core::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl<'a> Display for PathCow<'a> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::Borrowed(pr) => pr.fmt(f),
            Self::Owned(pv) => pv.fmt(f),
        }
    }
}

#[cfg(test)]
mod test {
    use alloc::string::ToString;

    use crate::write::fs::{PathCow, PathRef, PathVec};

    #[test]
    fn test_pathcow_eq_pathcow() {
        let p1: PathRef = "/abc/def".into();
        let p1: PathCow = p1.into();
        let p2: PathRef = "/abc/def".into();
        let p2: PathCow = p2.into();

        assert_eq!(p1, p2);
    }

    #[test]
    fn test_pathcow_eq_pathref() {
        let p1: PathRef = "/abc/def".into();
        let p1: PathCow = p1.into();
        let p2: PathRef = "/abc/def".into();

        assert_eq!(p1, p2);
    }

    #[test]
    fn test_pathcow_eq_pathvec() {
        let p1: PathRef = "/abc/def".into();
        let p1: PathCow = p1.into();
        let p2: PathRef = "/abc/def".into();
        let p2: PathVec = p2.into();

        assert_eq!(p1, p2);
    }

    #[test]
    fn test_pathcow_cmp() {
        let p1: PathRef = "/abc/def".into();
        let p1: PathVec = p1.into();
        let p1: PathCow = p1.into();
        let p2: PathRef = "/abc/def".into();
        let p2: PathCow = p2.into();

        assert_eq!(p1.cmp(&p2), core::cmp::Ordering::Equal);
        assert_eq!(p1.partial_cmp(&p2), Some(core::cmp::Ordering::Equal));
    }

    #[test]
    fn test_pathcow_display_pathref() {
        let p1: PathRef = "/abc/def".into();
        let p1: PathCow = p1.into();

        assert_eq!(p1.to_string().as_str(), "/abc/def");
    }

    #[test]
    fn test_pathcow_display_pathvec() {
        let p1: PathRef = "/abc/def".into();
        let p1: PathVec = p1.into();
        let p1: PathCow = p1.into();

        assert_eq!(p1.to_string().as_str(), "/abc/def");
    }

    #[test]
    fn test_pathcow_into_pathref_ref() {
        let p1: PathRef = "/abc/def".into();
        let p1_cow: PathCow = p1.into();

        assert_eq!(p1, p1_cow.path_ref());
    }

    #[test]
    fn test_pathcow_into_pathref_vec() {
        let p1: PathRef = "/abc/def".into();
        let p1: PathVec = p1.into();
        let p1_cow: PathCow = p1.clone().into();

        assert_eq!(p1.as_path_ref(), p1_cow.path_ref());
    }

    #[test]
    fn test_pathcow_into_pathvec_ref() {
        let p1: PathRef = "/abc/def".into();
        let p1_cow: PathCow = p1.into();

        assert_eq!(p1, p1_cow.to_owned().as_path_ref());
    }

    #[test]
    fn test_pathcow_into_pathvec_vec() {
        let p1: PathRef = "/abc/def".into();
        let p1: PathVec = p1.into();
        let p1_cow: PathCow = p1.clone().into();

        assert_eq!(p1, p1_cow.to_owned());
    }
}
