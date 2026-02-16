use core::fmt::Display;
use core::iter::Filter;
use core::str::Split;

use alloc::borrow::ToOwned;
use alloc::string::String;
use alloc::vec::Vec;

use super::PathRef;

#[derive(Debug, Default, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct PathVec {
    inner: String,
}

pub type PathVecIter<'a> = Filter<Split<'a, &'static str>, for<'b> fn(&'b &'a str) -> bool>;

impl<'a> FromIterator<&'a str> for PathVec {
    fn from_iter<T: IntoIterator<Item = &'a str>>(iter: T) -> Self {
        let mut path = String::new();
        let mut iter = iter.into_iter().filter(|s| !s.is_empty());

        if let Some(component) = iter.next() {
            path.push_str(component);
        }

        for component in iter {
            path.push('/');
            path.push_str(component);
        }

        Self { inner: path }
    }
}

impl<'a> IntoIterator for &'a PathVec {
    type Item = &'a str;
    type IntoIter = PathVecIter<'a>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'a> From<&'a str> for PathVec {
    fn from(value: &'a str) -> Self {
        PathVec::from_iter(value.split("/"))
    }
}

impl Display for PathVec {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        use core::fmt::Write;

        f.write_char('/')?;
        f.write_str(self.inner.as_str())
    }
}

impl PathVec {
    pub fn iter<'a>(&'a self) -> PathVecIter<'a> {
        self.inner.split("/").filter(|x| !x.is_empty())
    }

    pub fn as_path_buf(&self, prefix: &std::path::Path) -> std::path::PathBuf {
        prefix.join(&self.inner)
    }

    pub fn is_root(&self) -> bool {
        self.inner.is_empty() || self.inner == "/"
    }

    pub fn from_base(mut prefix: Self, suffix: &str) -> Self {
        if !prefix.is_root() {
            prefix.inner.push('/');
        }

        prefix.inner.push_str(suffix);
        prefix
    }

    pub fn base(&self) -> Option<PathVec> {
        if self.is_root() {
            None
        } else {
            let last_component_split = self.inner.rfind('/').unwrap_or(0);
            let base_slice = self.inner[0..last_component_split].to_owned();
            Some(PathVec { inner: base_slice })
        }
    }

    pub fn suffix(&self, prefix: &Self) -> Self {
        let mut components = Vec::new();
        let mut i1 = self.iter();
        let mut i2 = prefix.iter();

        loop {
            let c1 = i1.next();
            let c2 = i2.next();

            if let Some(component) = c1 {
                if let Some(component2) = c2 {
                    assert_eq!(component, component2);
                } else {
                    components.push(component.to_owned());
                }
            } else {
                return Self {
                    inner: components.join("/"),
                };
            }
        }
    }

    pub fn as_path_ref(&self) -> PathRef<'_> {
        self.into()
    }
}

#[cfg(test)]
mod test {
    use super::PathVec;
    use alloc::borrow::ToOwned;

    #[test]
    fn test_pathvec_from_iter() {
        let path = &["hello", "world"];
        let path = PathVec::from_iter(path.iter().copied());
        let components: alloc::vec::Vec<alloc::string::String> =
            path.iter().map(|x| x.to_owned()).collect();

        assert_eq!(components, &["hello", "world",]);
    }

    #[test]
    fn test_pathvec_into_iterator() {
        let path = &["hello", "world"];
        let path = PathVec::from_iter(path.iter().copied());
        let components: alloc::vec::Vec<alloc::string::String> =
            path.into_iter().map(|x| x.to_owned()).collect();

        assert_eq!(components, &["hello", "world",]);
    }

    #[test]
    fn test_pathvec_from_str() {
        let path = "/hello/world";
        let path = PathVec::from(path);
        let components: alloc::vec::Vec<alloc::string::String> =
            path.iter().map(|x| x.to_owned()).collect();

        assert_eq!(components, &["hello", "world",]);
    }

    #[test]
    fn test_pathvec_is_root_root_path() {
        let path = PathVec::default();
        assert!(path.is_root());
    }

    #[test]
    fn test_pathvec_is_root_non_root_path() {
        let path = PathVec::default();
        let path = PathVec::from_base(path, "nonroot");
        assert!(!path.is_root());
    }

    #[test]
    fn test_pathvec_base_root() {
        assert_eq!(PathVec::default().base(), None);
    }

    #[test]
    fn test_pathvec_base_non_root() {
        let path = PathVec::default();
        let path = PathVec::from_base(path, "nonroot");

        assert_eq!(path.base(), Some(PathVec::default()));
    }

    #[test]
    fn test_pathvec_to_path_buf() {
        let path = PathVec::default();
        let path = PathVec::from_base(path, "nonroot");
        let base = std::path::PathBuf::from("test");

        let path = path.as_path_buf(&base);
        assert_eq!(path, base.join("nonroot"));
    }

    #[test]
    fn test_pathvec_suffix() {
        let root = PathVec::default();
        let prefix = PathVec::from_base(root, "foo");
        let path = PathVec::from_base(prefix.clone(), "bar");
        let path = PathVec::from_base(path, "baz");

        let suffix = path.suffix(&prefix);
        assert_eq!(suffix, PathVec::from_iter(["bar", "baz",].into_iter()));
    }

    #[test]
    fn test_pathvec_to_string() {
        use alloc::string::ToString;

        let root = PathVec::default();
        let path = PathVec::from_base(root, "hello");
        let path = PathVec::from_base(path, "world");

        assert_eq!(path.to_string(), "/hello/world");
    }

    #[test]
    fn test_pathvec_to_string_root() {
        use alloc::string::ToString;

        let root = PathVec::default();
        assert_eq!(root.to_string(), "/");
    }
}
