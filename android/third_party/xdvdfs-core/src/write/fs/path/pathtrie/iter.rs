use alloc::collections::VecDeque;
use alloc::string::String;

use crate::write::fs::path::PathPrefixTree;

pub struct PPTIter<'a, T> {
    queue: VecDeque<(String, &'a PathPrefixTree<T>)>,
}

impl<'a, T> Iterator for PPTIter<'a, T> {
    type Item = (String, &'a T);

    fn next(&mut self) -> Option<Self::Item> {
        use alloc::borrow::ToOwned;

        // Expand until we find a node with a record
        while let Some(subtree) = self.queue.pop_front() {
            let (name, node) = &subtree;
            for (ch, child) in node.children.iter() {
                if let Some(child) = child {
                    let mut name = name.to_owned();
                    name.push(ch);
                    self.queue.push_back((name, child));
                }
            }

            if let Some(record) = &node.record {
                return Some((name.to_owned(), &record.0));
            }
        }

        None
    }
}

impl<T> PathPrefixTree<T> {
    pub fn iter(&self) -> PPTIter<'_, T> {
        let mut queue = VecDeque::new();
        queue.push_back((String::new(), self));
        PPTIter { queue }
    }
}

#[cfg(test)]
mod test {
    use crate::write::fs::PathPrefixTree;

    #[test]
    fn test_ppt_iterator() {
        use alloc::string::{String, ToString};
        use alloc::vec::Vec;

        let mut ppt = PathPrefixTree::default();
        ppt.insert_tail("abc", 1).insert_tail("tail", 2);
        ppt.insert_tail("hjk", 3).insert_tail("tail", 4);
        ppt.insert_tail("xyz", 5).insert_tail("tail", 6);

        let values: Vec<(String, i64)> = ppt.iter().map(|(name, val)| (name, *val)).collect();

        assert_eq!(
            values,
            [
                ("abc".to_string(), 1),
                ("hjk".to_string(), 3),
                ("xyz".to_string(), 5),
            ]
        );
    }
}
