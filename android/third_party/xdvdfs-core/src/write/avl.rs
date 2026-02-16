use core::cmp::Ordering;

use alloc::vec::Vec;

#[repr(i8)]
#[derive(Copy, Clone)]
enum AvlDirection {
    Left = -1,
    Right = 1,
    Leaf = 0,
}

#[derive(Debug, Clone)]
pub struct AvlNode<T: Ord> {
    left_node: Option<usize>,
    right_node: Option<usize>,
    parent: Option<usize>,
    height: i32,
    data: T,
}

impl<T: Ord> AvlNode<T> {
    fn new(data: T, parent: Option<usize>) -> AvlNode<T> {
        AvlNode {
            left_node: None,
            right_node: None,
            parent,
            height: 1,
            data,
        }
    }

    fn child_from_direction(&self, dir: AvlDirection) -> Option<usize> {
        match dir {
            AvlDirection::Left => self.left_node,
            AvlDirection::Right => self.right_node,
            AvlDirection::Leaf => None,
        }
    }

    pub fn left_idx(&self) -> Option<usize> {
        self.left_node
    }

    pub fn right_idx(&self) -> Option<usize> {
        self.right_node
    }

    pub fn data(&self) -> &T {
        &self.data
    }

    pub fn data_mut(&mut self) -> &mut T {
        &mut self.data
    }
}

#[derive(Debug, Clone)]
pub struct AvlTree<T: Ord> {
    root: Option<usize>,
    tree: Vec<AvlNode<T>>,
}

impl<T: Ord> Default for AvlTree<T> {
    fn default() -> Self {
        Self {
            root: None,
            tree: Vec::new(),
        }
    }
}

impl<T: Ord> AvlTree<T> {
    #[cfg(test)]
    fn height_slow(&self, node: usize, memo: &mut [Option<i32>]) -> i32 {
        if let Some(height) = memo[node] {
            return height;
        }

        let left_height = match self.tree[node].left_node {
            Some(idx) => self.height_slow(idx, memo),
            None => 0,
        };

        let right_height = match self.tree[node].right_node {
            Some(idx) => self.height_slow(idx, memo),
            None => 0,
        };

        let height = 1 + core::cmp::max(left_height, right_height);
        memo[node] = Some(height);
        height
    }

    #[cfg(test)]
    fn validate_tree(&self) {
        struct NodeEntry {
            node: usize,
            prev: Option<usize>,
        }

        let Some(root) = self.root else {
            return;
        };

        let mut heights: Vec<Option<i32>> = Vec::new();
        heights.resize(self.tree.len(), None);

        let mut stack: Vec<NodeEntry> = Vec::new();
        stack.push(NodeEntry {
            node: root,
            prev: None,
        });

        while let Some(entry) = stack.pop() {
            let node = &self.tree[entry.node];
            assert_eq!(node.parent, entry.prev);
            assert_eq!(node.height, self.height_slow(entry.node, &mut heights));

            let bf = self.balance_factor(entry.node);
            assert!(bf <= 1);
            assert!(bf >= -1);

            if let Some(left) = node.left_node {
                assert_eq!(Ord::cmp(&self.tree[left].data, &node.data), Ordering::Less);
                stack.push(NodeEntry {
                    node: left,
                    prev: Some(entry.node),
                });
            }

            if let Some(right) = node.right_node {
                assert_eq!(
                    Ord::cmp(&self.tree[right].data, &node.data),
                    Ordering::Greater
                );
                stack.push(NodeEntry {
                    node: right,
                    prev: Some(entry.node),
                });
            }
        }
    }

    pub fn reserve(&mut self, size: usize) {
        self.tree.reserve(size);
    }

    fn allocate(&mut self, data: T, parent: Option<usize>) -> usize {
        self.tree.push(AvlNode::new(data, parent));
        self.tree.len() - 1
    }

    fn parent_direction(&self, node_idx: usize) -> Option<(usize, AvlDirection)> {
        let node = &self.tree[node_idx];
        let parent = node.parent?;
        let parent_node = &self.tree[parent];

        if let Some(idx) = parent_node.left_node {
            if idx == node_idx {
                return Some((parent, AvlDirection::Left));
            }
        }

        if let Some(idx) = parent_node.right_node {
            if idx == node_idx {
                return Some((parent, AvlDirection::Right));
            }
        }

        unreachable!("AVL node-parent invariant violated");
    }

    fn parent_node_ref(&mut self, idx: usize) -> &mut Option<usize> {
        let parent_dir = self.parent_direction(idx);
        let (parent_idx, dir) = match parent_dir {
            Some(val) => val,
            None => return &mut self.root,
        };

        let parent = &mut self.tree[parent_idx];
        match dir {
            AvlDirection::Left => &mut parent.left_node,
            AvlDirection::Right => &mut parent.right_node,
            AvlDirection::Leaf => unreachable!(),
        }
    }

    fn update_node_height(&mut self, node_idx: usize) {
        let node = &self.tree[node_idx];
        let left_height = match node.left_node {
            Some(idx) => self.tree[idx].height,
            None => 0,
        };

        let right_height = match node.right_node {
            Some(idx) => self.tree[idx].height,
            None => 0,
        };

        self.tree[node_idx].height = 1 + core::cmp::max(left_height, right_height);
    }

    fn balance_factor(&self, node_idx: usize) -> i32 {
        let node = &self.tree[node_idx];
        let left_height = match node.left_node {
            Some(idx) => self.tree[idx].height,
            None => 0,
        };

        let right_height = match node.right_node {
            Some(idx) => self.tree[idx].height,
            None => 0,
        };

        left_height - right_height
    }

    fn set_left_child(&mut self, node: usize, to: Option<usize>) {
        self.tree[node].left_node = to;
        if let Some(idx) = to {
            self.tree[idx].parent = Some(node);
        }
    }

    fn set_right_child(&mut self, node: usize, to: Option<usize>) {
        self.tree[node].right_node = to;
        if let Some(idx) = to {
            self.tree[idx].parent = Some(node);
        }
    }

    fn l_rotate(&mut self, a: usize, b: usize, c: usize) {
        assert_eq!(self.tree[a].right_node, Some(b));
        assert_eq!(self.tree[b].right_node, Some(c));

        let a_parent = self.tree[a].parent;
        *self.parent_node_ref(a) = Some(b);
        self.tree[b].parent = a_parent;

        let b_left = self.tree[b].left_node;
        self.set_right_child(a, b_left);
        self.set_left_child(b, Some(a));

        self.update_node_height(a);
        self.update_node_height(c);
        self.update_node_height(b);
    }

    fn r_rotate(&mut self, a: usize, b: usize, c: usize) {
        assert_eq!(self.tree[a].left_node, Some(b));
        assert_eq!(self.tree[b].left_node, Some(c));

        let a_parent = self.tree[a].parent;
        *self.parent_node_ref(a) = Some(b);
        self.tree[b].parent = a_parent;

        let b_right = self.tree[b].right_node;
        self.set_left_child(a, b_right);
        self.set_right_child(b, Some(a));

        self.update_node_height(a);
        self.update_node_height(c);
        self.update_node_height(b);
    }

    fn rl_rotate(&mut self, a: usize, b: usize, c: usize) {
        assert_eq!(self.tree[a].right_node, Some(b));
        assert_eq!(self.tree[b].left_node, Some(c));

        let a_parent = self.tree[a].parent;
        *self.parent_node_ref(a) = Some(c);
        self.tree[c].parent = a_parent;

        let c_left = self.tree[c].left_node;
        self.set_right_child(a, c_left);

        let c_right = self.tree[c].right_node;
        self.set_left_child(b, c_right);

        self.set_right_child(c, Some(b));
        self.set_left_child(c, Some(a));

        self.update_node_height(a);
        self.update_node_height(b);
        self.update_node_height(c);
    }

    fn lr_rotate(&mut self, a: usize, b: usize, c: usize) {
        assert_eq!(self.tree[a].left_node, Some(b));
        assert_eq!(self.tree[b].right_node, Some(c));

        let a_parent = self.tree[a].parent;
        *self.parent_node_ref(a) = Some(c);
        self.tree[c].parent = a_parent;

        let c_left = self.tree[c].left_node;
        self.set_right_child(b, c_left);

        let c_right = self.tree[c].right_node;
        self.set_left_child(a, c_right);

        self.set_right_child(c, Some(a));
        self.set_left_child(c, Some(b));

        self.update_node_height(a);
        self.update_node_height(b);
        self.update_node_height(c);
    }

    fn perform_rotation(
        &mut self,
        a: usize,
        b: usize,
        c: usize,
        ab: AvlDirection,
        bc: AvlDirection,
    ) {
        match ab {
            AvlDirection::Left => match bc {
                AvlDirection::Left => self.r_rotate(a, b, c),
                AvlDirection::Right => self.lr_rotate(a, b, c),
                AvlDirection::Leaf => unreachable!("AVL Rotate involves leaf child"),
            },
            AvlDirection::Right => match bc {
                AvlDirection::Left => self.rl_rotate(a, b, c),
                AvlDirection::Right => self.l_rotate(a, b, c),
                AvlDirection::Leaf => unreachable!("AVL Rotate involves leaf child"),
            },
            AvlDirection::Leaf => unreachable!("AVL Rotate involves leaf child"),
        }
    }

    fn rebalance(&mut self, leaf: usize) {
        assert!(self
            .tree
            .get(leaf)
            .is_some_and(|node| node.left_node.is_none() && node.right_node.is_none()));

        let mut current_idx = Some((leaf, AvlDirection::Leaf));
        let mut prev_idx: Option<(usize, AvlDirection)> = None;
        while let Some((idx, dir)) = current_idx {
            self.update_node_height(idx);
            let balance_factor = self.balance_factor(idx);

            if !(-1..=1).contains(&balance_factor) {
                let (node_b, prev_dir) = prev_idx.expect("leaf node will not be unbalanced");
                let node_c = self.tree[node_b]
                    .child_from_direction(prev_dir)
                    .expect("child existence should be guaranteed by balance factor");
                self.perform_rotation(idx, node_b, node_c, dir, prev_dir);
            }

            prev_idx = current_idx;
            current_idx = self.parent_direction(idx);
        }
    }

    /// Inserts an element into the tree
    /// Returns true if that element was new (i.e. did not exist in the tree already)
    /// On duplicate insert, the value is not modified
    pub fn insert(&mut self, data: T) -> bool {
        // This makes an assumption that new elements are always
        // allocated at the end of the backing vector.
        let next_free_index = self.tree.len();

        let mut current_node = match self.root {
            Some(idx) => idx,
            None => {
                let new_idx = self.allocate(data, None);
                assert_eq!(next_free_index, new_idx);
                self.root = Some(next_free_index);

                return true;
            }
        };

        let mut prev_node;
        loop {
            prev_node = Some(current_node);
            let node = &mut self.tree[current_node];
            let cmp = Ord::cmp(&data, &node.data);
            let next_node = match cmp {
                Ordering::Less => &mut node.left_node,
                Ordering::Greater => &mut node.right_node,
                Ordering::Equal => return false,
            };

            match next_node {
                Some(idx) => current_node = *idx,
                None => {
                    *next_node = Some(next_free_index);
                    break;
                }
            }
        }

        let new_element_index = self.allocate(data, prev_node);
        assert_eq!(next_free_index, new_element_index);
        self.rebalance(new_element_index);
        true
    }

    #[cfg(test)]
    pub fn inorder_iter(&self) -> AvlInorderIter<'_, T> {
        let mut stack = Vec::new();
        let mut current_node = self.root;

        while let Some(node) = current_node {
            stack.push(node);
            current_node = self.tree[node].left_node;
        }

        AvlInorderIter { stack, tree: self }
    }

    pub fn preorder_iter(&self) -> AvlPreorderIter<'_, T> {
        let mut stack = Vec::new();
        if let Some(idx) = self.root {
            stack.push(idx);
        }

        AvlPreorderIter { stack, tree: self }
    }

    pub fn fold_mut<V>(&mut self, mut acc: V, mut f: impl FnMut(V, &mut T) -> V) -> V {
        for node in self.tree.iter_mut() {
            acc = f(acc, &mut node.data);
        }

        acc
    }

    pub fn backing_vec(&self) -> &Vec<AvlNode<T>> {
        &self.tree
    }

    pub fn backing_vec_mut(&mut self) -> &mut Vec<AvlNode<T>> {
        &mut self.tree
    }

    pub fn len(&self) -> usize {
        self.tree.len()
    }
}

pub struct AvlNodeRef<'tree, T: Ord> {
    node: usize,
    tree: &'tree AvlTree<T>,
}

impl<T: Ord> AvlNodeRef<'_, T> {
    pub fn backing_index(&self) -> usize {
        self.node
    }
}

impl<T: Ord> core::ops::Deref for AvlNodeRef<'_, T> {
    type Target = T;
    fn deref(&self) -> &Self::Target {
        &self.tree.tree[self.node].data
    }
}

#[cfg(test)]
pub struct AvlInorderIter<'tree, T: Ord> {
    stack: Vec<usize>,
    tree: &'tree AvlTree<T>,
}

#[cfg(test)]
impl<'tree, T: Ord> core::iter::Iterator for AvlInorderIter<'tree, T> {
    type Item = AvlNodeRef<'tree, T>;
    fn next(&mut self) -> Option<Self::Item> {
        let top = self.stack.pop()?;

        let mut next = self.tree.tree[top].right_node;
        while let Some(node) = next {
            self.stack.push(node);
            next = self.tree.tree[node].left_node;
        }

        Some(AvlNodeRef {
            node: top,
            tree: self.tree,
        })
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let tree_len = self.tree.backing_vec().len();
        (tree_len, Some(tree_len))
    }
}

pub struct AvlPreorderIter<'tree, T: Ord> {
    stack: Vec<usize>,
    tree: &'tree AvlTree<T>,
}

impl<'tree, T: Ord> core::iter::Iterator for AvlPreorderIter<'tree, T> {
    type Item = AvlNodeRef<'tree, T>;
    fn next(&mut self) -> Option<Self::Item> {
        let top = self.stack.pop()?;

        let node = &self.tree.tree[top];
        if let Some(idx) = node.right_node {
            self.stack.push(idx);
        }

        if let Some(idx) = node.left_node {
            self.stack.push(idx);
        }

        Some(AvlNodeRef {
            node: top,
            tree: self.tree,
        })
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let tree_len = self.tree.backing_vec().len();
        (tree_len, Some(tree_len))
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use rand::{prelude::*, rngs, SeedableRng};

    #[test]
    fn test_avl_tree_insert_invariants() {
        let mut rng = rngs::StdRng::seed_from_u64(0x5842_4f58_5842_4f58);
        let mut test_set: Vec<i32> = Vec::new();
        test_set.resize_with(1000, || rng.gen());

        let mut tree = AvlTree::default();

        for i in test_set {
            let insert_result = tree.insert(i);
            assert!(insert_result);
            tree.validate_tree();
        }
    }

    #[test]
    fn test_avl_tree_duplicate_insert() {
        let mut tree = AvlTree::default();

        assert!(tree.insert(10));
        assert!(!tree.insert(10));
    }

    #[test]
    fn test_avl_tree_inorder_ordering() {
        let mut rng = rngs::StdRng::seed_from_u64(0x5842_4f58_5842_4f58);
        let mut test_set: Vec<i32> = Vec::new();
        test_set.resize_with(1000, || rng.gen());

        let mut tree = AvlTree::default();
        let mut btree = std::collections::BTreeSet::new();
        tree.reserve(1000);

        for i in test_set {
            tree.insert(i);
            btree.insert(i);
            tree.validate_tree();
        }

        for (x, y) in btree.iter().zip(tree.inorder_iter()) {
            assert_eq!(*x, *y);
        }
    }

    #[test]
    fn test_avl_tree_preorder_ordering() {
        let test_set = [1, 2, 3, 4, 5, 6];

        let mut tree = AvlTree::default();
        tree.reserve(6);

        for i in test_set {
            tree.insert(i);
            tree.validate_tree();
        }

        let preorder: Vec<i32> = tree.preorder_iter().map(|n| *n).collect();
        assert_eq!(preorder, [4, 2, 1, 3, 5, 6]);
    }

    #[test]
    fn test_avl_tree_inorder_size_hint() {
        let test_set = [1, 2, 3, 4, 5, 6];

        let mut tree = AvlTree::default();
        tree.reserve(6);

        for i in test_set {
            tree.insert(i);
            tree.validate_tree();
        }

        let iter = tree.inorder_iter();
        assert_eq!(iter.size_hint(), (6, Some(6)));
    }

    #[test]
    fn test_avl_tree_preorder_size_hint() {
        let test_set = [1, 2, 3, 4, 5, 6];

        let mut tree = AvlTree::default();
        tree.reserve(6);

        for i in test_set {
            tree.insert(i);
            tree.validate_tree();
        }

        let iter = tree.preorder_iter();
        assert_eq!(iter.size_hint(), (6, Some(6)));
    }

    #[test]
    fn test_avl_tree_fold_mut() {
        let test_set = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];

        let mut tree = AvlTree::default();
        tree.reserve(10);

        for i in test_set {
            tree.insert(i);
            tree.validate_tree();
        }

        let val = tree.fold_mut(0, |mut acc, elem| {
            acc += *elem;
            *elem += 1;
            acc
        });
        assert_eq!(val, 55);

        let data: Vec<i32> = tree.backing_vec().iter().map(|x| x.data).collect();
        assert_eq!(data, &[2, 3, 4, 5, 6, 7, 8, 9, 10, 11,]);
    }
}
