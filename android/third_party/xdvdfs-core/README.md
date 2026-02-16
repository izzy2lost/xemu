# xdvdfs-core

`xdvdfs-core` is a library for working with XDVDFS metadata.

A simple example reading a file from a given path is:

```rust
async fn read_from_path(xiso: &Path, file_path: &str) -> Box<[u8]> {
    let mut xiso = std::fs::File::open(xiso).unwrap();
    let volume = xdvdfs::read::read_volume(&mut xiso).await.unwrap();

    let file_dirent = volume.root_table.walk_path(&mut xiso, file_path).await.unwrap();

    let data = file_dirent.node.dirent.read_data_all(&mut xiso).await.unwrap();
    data
}
```

This library supports no_std. Custom block devices can be defined by implementing the traits in `xdvdfs::blockdev`.

Without the `alloc` feature, only basic metadata features are supported. The `alloc` feature enables several utility
functions that require allocation (such as `read_data_all` above.

The source code for xdvdfs-cli provides a more detailed example of how to use xdvdfs-core in an environment with std.

Note that xdvdfs is currently not API stable, and following semver with major version 0, each minor version bump may or
may not include breaking changes.
