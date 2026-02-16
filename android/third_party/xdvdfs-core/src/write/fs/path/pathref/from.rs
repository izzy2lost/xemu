use crate::write::fs::PathVec;

use super::PathRef;

impl<'a> From<&'a str> for PathRef<'a> {
    fn from(value: &'a str) -> Self {
        PathRef::Str(value)
    }
}

impl<'a> From<&'a [&'a str]> for PathRef<'a> {
    fn from(value: &'a [&'a str]) -> Self {
        PathRef::Slice(value)
    }
}

impl<'a> From<&'a PathVec> for PathRef<'a> {
    fn from(value: &'a PathVec) -> Self {
        PathRef::PathVec(value)
    }
}

impl<'a> From<PathRef<'a>> for PathVec {
    fn from(value: PathRef<'a>) -> Self {
        match value {
            PathRef::Str(s) => PathVec::from(s),
            PathRef::Slice(sl) => PathVec::from_iter(sl.iter().map(|s| &**s)),
            PathRef::PathVec(pv) => pv.clone(),
            PathRef::Join(base, tail) => PathVec::from_base(PathVec::from(*base), tail),
        }
    }
}

#[cfg(test)]
mod test {
    use super::PathRef;

    #[test]
    fn test_str_to_pathref() {
        use alloc::borrow::ToOwned;

        let path = "/hello/world/";
        let path: PathRef = path.into();
        let components: alloc::vec::Vec<alloc::string::String> =
            path.iter().map(|component| component.to_owned()).collect();

        assert_eq!(components, &["hello", "world",]);
    }

    #[test]
    fn test_slice_to_pathref() {
        use alloc::borrow::ToOwned;

        let path = &["hello", "world"].as_slice();
        let path: PathRef = (*path).into();
        let components: alloc::vec::Vec<alloc::string::String> =
            path.iter().map(|component| component.to_owned()).collect();

        assert_eq!(components, &["hello", "world",]);
    }

    #[test]
    fn test_pathvec_to_pathref() {
        use super::PathVec;
        use alloc::borrow::ToOwned;

        let path = PathVec::from_base(PathVec::default(), "hello");
        let path = PathVec::from_base(path.clone(), "world");
        let path: PathRef = path.as_path_ref();
        let components: alloc::vec::Vec<alloc::string::String> =
            path.iter().map(|x| x.to_owned()).collect();

        assert_eq!(components, &["hello", "world",]);
    }

    #[test]
    fn test_pathref_to_pathvec_string() {
        use alloc::borrow::ToOwned;

        let path = "/hello/world";
        let path: PathRef = path.into();
        let path: super::PathVec = path.into();
        let components: alloc::vec::Vec<alloc::string::String> =
            path.iter().map(|x| x.to_owned()).collect();

        assert_eq!(components, &["hello", "world",]);
    }

    #[test]
    fn test_pathref_to_pathvec_slice() {
        use alloc::borrow::ToOwned;

        let path = &["hello", "world"].as_slice();
        let path: PathRef = (*path).into();
        let path: super::PathVec = path.into();
        let components: alloc::vec::Vec<alloc::string::String> =
            path.iter().map(|x| x.to_owned()).collect();

        assert_eq!(components, &["hello", "world",]);
    }

    #[test]
    fn test_pathref_to_pathvec_pathvec() {
        use alloc::borrow::ToOwned;

        let path = super::PathVec::from("/hello/world");
        let path: PathRef = path.as_path_ref();
        let path: super::PathVec = path.into();
        let components: alloc::vec::Vec<alloc::string::String> =
            path.iter().map(|x| x.to_owned()).collect();

        assert_eq!(components, &["hello", "world",]);
    }
}
