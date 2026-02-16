use core::iter::FusedIterator;

use super::{
    iter::{BasePathRefIter, PathRefIter},
    PathRef,
};

pub struct JoinedPathRefIter<'a> {
    joined_component_stack: alloc::vec::Vec<&'a str>,
    base_iter: BasePathRefIter<'a>,
}

impl<'a> JoinedPathRefIter<'a> {
    pub fn new(base: &'a PathRef<'a>, tail: &'a str) -> Self {
        let mut stack = alloc::vec::Vec::new();
        stack.push(tail);

        let mut base = base;
        while let PathRef::Join(b, t) = base {
            stack.push(t);
            base = b;
        }

        let base_iter = match base.iter() {
            PathRefIter::Base(iter) => iter,
            _ => unreachable!("base is not a Join"),
        };

        Self {
            joined_component_stack: stack,
            base_iter,
        }
    }
}

impl<'a> Iterator for JoinedPathRefIter<'a> {
    type Item = &'a str;

    fn next(&mut self) -> Option<Self::Item> {
        if let Some(next) = self.base_iter.next() {
            return Some(next);
        }

        self.joined_component_stack.pop()
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let base_size_hint = self.base_iter.size_hint();
        (
            base_size_hint.0 + self.joined_component_stack.len(),
            base_size_hint
                .1
                .map(|x| x + self.joined_component_stack.len()),
        )
    }
}

impl FusedIterator for JoinedPathRefIter<'_> {}

#[cfg(test)]
mod test {
    use crate::write::fs::PathRef;

    #[test]
    fn test_pathref_iter_join_single() {
        use alloc::borrow::ToOwned;

        let path = "/hello";
        let path: PathRef = path.into();
        let path = path.join("world");
        let components: alloc::vec::Vec<alloc::string::String> =
            path.iter().map(|x| x.to_owned()).collect();

        assert_eq!(components, &["hello", "world"]);
    }

    #[test]
    fn test_pathref_iter_join_depth() {
        use alloc::borrow::ToOwned;

        let path = "/hello";
        let path: PathRef = path.into();
        let path = path.join("world");
        let path = path.join("abc");
        let components: alloc::vec::Vec<alloc::string::String> =
            path.iter().map(|x| x.to_owned()).collect();

        assert_eq!(components, &["hello", "world", "abc"]);
    }
}
