use crate::write::fs::PathVec;

mod cmp;
mod display;
mod eq;
mod from;
mod iter;
mod joined;

/// Opaque reference to a path
/// Internally implemented as an enum of path implementation variants
#[derive(Debug, Clone, Copy, Eq, Hash)]
pub enum PathRef<'a> {
    // String with '/' separator
    Str(&'a str),

    // Slice of components
    Slice(&'a [&'a str]),

    // PathVec reference
    PathVec(&'a PathVec),

    // PathRef joined with a string component
    Join(&'a PathRef<'a>, &'a str),
}

impl<'a> PathRef<'a> {
    pub fn join(&'a self, tail: &'a str) -> Self {
        Self::Join(self, tail)
    }

    pub fn as_path_buf(&self, prefix: &std::path::Path) -> std::path::PathBuf {
        match self {
            Self::Str(s) => {
                let s = if let Some(stripped) = s.strip_prefix('/') {
                    stripped
                } else {
                    s
                };

                prefix.join(s)
            }
            Self::PathVec(pv) => pv.as_path_buf(prefix),
            Self::Join(base, tail) => {
                let base = base.as_path_buf(prefix);
                base.join(tail)
            }
            other => {
                use alloc::string::ToString;

                // Trim the `/` to ensure path is relative.
                let path = other.to_string();
                debug_assert!(path.starts_with('/'));
                prefix.join(&path[1..])
            }
        }
    }

    pub fn is_root(&self) -> bool {
        match self {
            Self::Str(s) => s.is_empty() || *s == "/",
            Self::Slice(sl) => sl.is_empty(),
            Self::PathVec(pv) => pv.is_root(),
            Self::Join(_, _) => false,
        }
    }
}

#[cfg(test)]
mod test {
    use crate::write::fs::PathVec;

    use super::PathRef;

    #[test]
    fn test_pathref_to_pathvec_join() {
        use alloc::borrow::ToOwned;

        let path = "/hello/world";
        let path: PathRef = path.into();
        let path = path.join("abc");
        let path: super::PathVec = path.into();
        let components: alloc::vec::Vec<alloc::string::String> =
            path.iter().map(|x| x.to_owned()).collect();

        assert_eq!(components, &["hello", "world", "abc"]);
    }

    #[test]
    fn test_pathref_to_std_pathbuf_str() {
        use std::path::{Path, PathBuf};

        let base = PathBuf::from("/base/path");
        let path: PathRef = "/joined/path".into();
        let path = path.as_path_buf(&base);

        assert_eq!(path, Path::new("/base/path/joined/path"));
    }

    #[test]
    fn test_pathref_to_std_pathbuf_str_no_leading_slash() {
        use std::path::{Path, PathBuf};

        let base = PathBuf::from("/base/path");
        let path: PathRef = "joined/path".into();
        let path = path.as_path_buf(&base);

        assert_eq!(path, Path::new("/base/path/joined/path"));
    }

    #[test]
    fn test_pathref_to_std_pathbuf_slice() {
        use std::path::{Path, PathBuf};

        let base = PathBuf::from("/base/path");
        let path: PathRef = ["joined", "path"].as_slice().into();
        let path = path.as_path_buf(&base);

        assert_eq!(path, Path::new("/base/path/joined/path"));
    }

    #[test]
    fn test_pathref_to_std_pathbuf_vec() {
        use std::path::{Path, PathBuf};

        let base = PathBuf::from("/base/path");
        let path: PathRef = "/joined/path".into();
        let path: PathVec = path.into();
        let path = path.as_path_ref();
        let path = path.as_path_buf(&base);

        assert_eq!(path, Path::new("/base/path/joined/path"));
    }

    #[test]
    fn test_pathref_is_root_str() {
        let root_empty: PathRef = "".into();
        let root: PathRef = "/".into();
        let non_root: PathRef = "/abc".into();

        assert!(root_empty.is_root());
        assert!(root.is_root());
        assert!(!non_root.is_root());
    }

    #[test]
    fn test_pathref_is_root_slice() {
        let root: PathRef = [].as_slice().into();
        let non_root: PathRef = ["abc"].as_slice().into();

        assert!(root.is_root());
        assert!(!non_root.is_root());
    }

    #[test]
    fn test_pathref_is_root_pathvec() {
        use crate::write::fs::path::PathVec;

        let root = PathVec::default();
        let non_root = PathVec::from_base(root.clone(), "abc");
        let non_root: PathRef = non_root.as_path_ref();
        let root: PathRef = root.as_path_ref();

        assert!(root.is_root());
        assert!(!non_root.is_root());
    }

    #[test]
    fn test_pathref_is_root_joined() {
        let root: PathRef = "/".into();
        let non_root: PathRef = root.join("abc");

        assert!(!non_root.is_root());
    }
}
