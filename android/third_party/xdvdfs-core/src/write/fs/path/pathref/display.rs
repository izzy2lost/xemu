use core::fmt::{Display, Write};

use super::PathRef;

impl Display for PathRef<'_> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match *self {
            Self::Str(s) => {
                if s.is_empty() || !s.starts_with('/') {
                    f.write_char('/')?;
                }

                f.write_str(s)
            }
            Self::Slice([]) => f.write_char('/'),
            Self::Slice(sl) => {
                for component in sl {
                    f.write_char('/')?;
                    f.write_str(component)?;
                }

                Ok(())
            }
            Self::PathVec(pv) => pv.fmt(f),
            Self::Join(base, tail) => {
                base.fmt(f)?;
                f.write_char('/')?;
                f.write_str(tail)
            }
        }
    }
}

#[cfg(test)]
mod test {
    use crate::write::fs::path::PathRef;
    use alloc::string::ToString;

    #[test]
    fn test_pathref_display_str_root() {
        let empty_root: PathRef = "".into();
        let root: PathRef = "/".into();

        assert_eq!(empty_root.to_string(), "/");
        assert_eq!(root.to_string(), "/");
    }

    #[test]
    fn test_pathref_display_str() {
        let path: PathRef = "/abc/def".into();
        assert_eq!(path.to_string(), "/abc/def");
    }

    #[test]
    fn test_pathref_display_slice_root() {
        let path: PathRef = [].as_slice().into();
        assert_eq!(path.to_string(), "/");
    }

    #[test]
    fn test_pathref_display_slice() {
        let path: PathRef = ["abc", "def"].as_slice().into();
        assert_eq!(path.to_string(), "/abc/def");
    }

    #[test]
    fn test_pathref_display_pathvec() {
        use crate::write::fs::path::PathVec;

        let path = PathVec::default();
        let path = PathVec::from_base(path, "abc");
        let path = PathVec::from_base(path, "def");
        let path: PathRef = path.as_path_ref();
        assert_eq!(path.to_string(), "/abc/def");
    }

    #[test]
    fn test_pathref_display_join() {
        let path: PathRef = "/abc".into();
        let path = path.join("def");
        assert_eq!(path.to_string(), "/abc/def");
    }
}
