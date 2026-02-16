use alloc::boxed::Box;

use crate::write::fs::PathRef;

mod subtree;
use subtree::PPTSubtree;

mod iter;
mod subtree_iter;

pub use iter::PPTIter;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PathPrefixTree<T> {
    children: PPTSubtree<T>,
    pub record: Option<(T, Box<PathPrefixTree<T>>)>,
}

impl<T> Default for PathPrefixTree<T> {
    fn default() -> Self {
        Self {
            children: PPTSubtree::Empty,
            record: None,
        }
    }
}

impl<T> PathPrefixTree<T> {
    /// Looks up a node, only descending into subdirs if the path is not consumed
    pub fn lookup_node<'a, P: Into<PathRef<'a>>>(&self, path: P) -> Option<&Self> {
        let mut node = self;

        let mut component_iter = path.into().iter().peekable();
        while let Some(component) = component_iter.next() {
            for ch in component.chars() {
                let next = &node.children[ch];
                node = next.as_ref()?;
            }

            if component_iter.peek().is_some() {
                let record = &node.record;
                let (_, subtree) = record.as_ref()?;
                node = subtree;
            }
        }

        Some(node)
    }

    /// Looks up a node, only descending into subdirs if the path is not consumed
    pub fn lookup_node_mut<'a, P: Into<PathRef<'a>>>(&mut self, path: P) -> Option<&mut Self> {
        let mut node = self;

        let mut component_iter = path.into().iter().peekable();
        while let Some(component) = component_iter.next() {
            for ch in component.chars() {
                let next = &mut node.children[ch];
                node = next.as_mut()?;
            }

            if component_iter.peek().is_some() {
                let record = &mut node.record;
                let (_, subtree) = record.as_mut()?;
                node = subtree;
            }
        }

        Some(node)
    }

    /// Looks up a subdir, returning its subtree
    pub fn lookup_subdir<'a, P: Into<PathRef<'a>>>(&self, path: P) -> Option<&Self> {
        let mut node = self;

        for component in path.into().iter() {
            for ch in component.chars() {
                let next = &node.children[ch];
                node = next.as_ref()?;
            }

            let record = &node.record;
            let (_, subtree) = record.as_ref()?;
            node = subtree;
        }

        Some(node)
    }

    pub fn insert_tail(&mut self, tail: &str, val: T) -> &mut Self {
        let mut node = self;

        for ch in tail.chars() {
            let next = &mut node.children[ch];
            node = next.get_or_insert_with(|| Box::new(Self::default()));
        }

        let mut subtree = Box::new(Self::default());
        if let Some(record) = &mut node.record {
            // We can't replace `val` and return because of a bug,
            // so swap out the subtree for a blank one, and then
            // allow the record to be re-inserted with `val` if
            // something is already present.
            // This preserves the subtree and sets the new value.
            core::mem::swap(&mut record.1, &mut subtree);
            node.record = None;
        }

        node.record.get_or_insert((val, subtree)).1.as_mut()
    }

    pub fn get<'a, P: Into<PathRef<'a>>>(&self, path: P) -> Option<&T> {
        let node = self.lookup_node(path)?;
        node.record.as_ref().map(|v| &v.0)
    }
}

impl<T: Default> PathPrefixTree<T> {
    pub fn insert_path<'a, P: Into<PathRef<'a>>>(&mut self, path: P, val: T) {
        let mut node = self;
        let path: PathRef = path.into();

        if path.is_root() {
            node.insert_tail("", val);
            return;
        }

        let mut iter = path.iter().peekable();
        while let Some(component) = iter.next() {
            if iter.peek().is_some() {
                node = node.insert_tail(component, T::default());
            } else {
                node.insert_tail(component, val);
                break;
            }
        }
    }
}

#[cfg(test)]
mod test {
    use crate::write::fs::PathPrefixTree;

    #[test]
    fn test_ppt_insert_get() {
        let mut ppt = PathPrefixTree::default();
        let tail = ppt.insert_tail("azbxcy", 12345);

        // Tail is a new PPT
        assert!(tail.record.is_none());
        assert!(tail.children.iter().next().is_none());

        assert_eq!(ppt.get("azbxcy"), Some(&12345));
    }

    #[test]
    fn test_ppt_lookup_tail() {
        let mut ppt = PathPrefixTree::default();

        ppt.insert_tail("foo", 12345).insert_tail("bar", 67890);

        let (val, subtree) = ppt
            .lookup_node("foo")
            .expect("Node 'foo' should have been inserted")
            .record
            .as_ref()
            .expect("Node 'foo' should have a record");
        assert_eq!(*val, 12345);
        assert_eq!(subtree.get("bar"), Some(&67890));
    }

    #[test]
    fn test_ppt_lookup_node() {
        let mut ppt = PathPrefixTree::default();

        ppt.insert_tail("foo", 12345).insert_tail("bar", 67890);

        let (val, subtree) = ppt
            .lookup_node("foo/bar")
            .expect("Node 'foo/bar' should have been inserted")
            .record
            .as_ref()
            .expect("Node 'foo/bar' should have a record");
        assert_eq!(*val, 67890);
        assert!(subtree.children.iter().next().is_none());
    }

    #[test]
    fn test_ppt_lookup_node_no_entry() {
        let mut ppt = PathPrefixTree::default();

        ppt.insert_tail("foo", 12345).insert_tail("bar", 67890);

        assert!(ppt.lookup_node("foo/baz").is_none());
    }

    #[test]
    fn test_ppt_lookup_node_no_subtree() {
        let mut ppt = PathPrefixTree::default();

        ppt.insert_tail("foo", 12345);

        // First component has `fo` instead of `foo`,
        // so there is no subtree.
        assert!(ppt.lookup_node("fo/bar").is_none());
    }

    #[test]
    fn test_insert_tail_replace_value() {
        let mut ppt = PathPrefixTree::default();

        ppt.insert_tail("foo", 12345);
        assert_eq!(ppt.get("/foo"), Some(&12345));

        ppt.insert_tail("foo", 67890);
        assert_eq!(ppt.get("/foo"), Some(&67890));
    }

    #[test]
    fn test_insert_path() {
        let mut ppt: PathPrefixTree<Option<i32>> = PathPrefixTree::default();

        ppt.insert_path("/a/b/c", Some(1234));
        ppt.insert_path("/a/b", Some(6789));
        ppt.insert_path("/", Some(4321));

        assert_eq!(ppt.get(""), Some(&Some(4321)));
        assert_eq!(ppt.get("/a/b"), Some(&Some(6789)));
        assert_eq!(ppt.get("/a/b/c"), Some(&Some(1234)));
    }

    #[test]
    fn test_ppt_get_node() {
        let mut ppt = PathPrefixTree::default();

        ppt.insert_tail("foo", 12345).insert_tail("bar", 67890);

        assert_eq!(ppt.get("foo/bar"), Some(&67890));
    }

    #[test]
    fn test_ppt_get_node_no_entry() {
        let mut ppt = PathPrefixTree::default();

        ppt.insert_tail("foo", 12345).insert_tail("bar", 67890);

        assert_eq!(ppt.get("foo/baz"), None);
    }

    #[test]
    fn test_ppt_lookup_subdir() {
        let mut ppt = PathPrefixTree::default();

        ppt.insert_tail("foo", 1)
            .insert_tail("bar", 2)
            .insert_tail("baz", 3);

        let subtree = ppt
            .lookup_subdir("foo/bar")
            .expect("Node 'foo/bar' should exist");
        assert_eq!(subtree.get("baz"), Some(&3));
    }

    #[test]
    fn test_ppt_lookup_subdir_no_entry() {
        let mut ppt = PathPrefixTree::default();

        ppt.insert_tail("foo", 1).insert_tail("bar", 2);

        assert!(ppt.lookup_subdir("foo/baz").is_none());
    }

    #[test]
    fn test_ppt_lookup_subdir_no_subtree() {
        let mut ppt = PathPrefixTree::default();

        ppt.insert_tail("foo", 1).insert_tail("bar", 2);

        // Second component has `ba` instead of `baz`,
        // so there is no subtree
        assert!(ppt.lookup_subdir("foo/ba/baz").is_none());
    }

    #[test]
    fn test_ppt_substring_path() {
        let mut ppt = PathPrefixTree::default();

        ppt.insert_tail("abcdef", 12345);
        ppt.insert_tail("abc", 67890);

        assert_eq!(ppt.get("abc"), Some(&67890));
        assert_eq!(ppt.get("abcdef"), Some(&12345));
    }

    #[test]
    fn test_ppt_lookup_node_mut_mutate_record() {
        let mut ppt = PathPrefixTree::default();
        ppt.insert_tail("abc", 54321).insert_tail("def", 12345);

        let record = &mut ppt
            .lookup_node_mut("abc/def")
            .expect("Node 'abc/def' should exist")
            .record
            .as_mut()
            .expect("Node 'abc/def' should have a record");
        record.0 = 67890;

        assert_eq!(ppt.get("abc/def"), Some(&67890));
    }

    #[test]
    fn test_ppt_lookup_node_mut_no_record() {
        let mut ppt = PathPrefixTree::default();
        ppt.insert_tail("abc", 54321).insert_tail("def", 12345);

        assert!(ppt.lookup_node_mut("abc/defg").is_none());
    }

    #[test]
    fn test_ppt_lookup_node_mut_no_subtree() {
        let mut ppt = PathPrefixTree::default();
        ppt.insert_tail("abc", 54321).insert_tail("def", 12345);

        // Second component has `de` instead of `def`,
        // so there is no subtree
        assert!(ppt.lookup_node_mut("abc/de/ghi").is_none());
    }
}
