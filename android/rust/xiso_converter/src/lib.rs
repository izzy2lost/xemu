use std::ffi::{c_char, CStr};
use std::fs::File;
use std::io::{BufReader, BufWriter, Write};
use std::path::{Path, PathBuf};

use anyhow::{anyhow, bail, Context};
use xdvdfs::blockdev::OffsetWrapper;
use xdvdfs::write::fs::{StdIOCopier, XDVDFSFilesystem};
use xdvdfs::write::img::{create_xdvdfs_image, NoOpProgressVisitor};

fn canonical_or_absolute(path: &Path) -> std::io::Result<PathBuf> {
    match std::fs::canonicalize(path) {
        Ok(p) => Ok(p),
        Err(_) => {
            if path.is_absolute() {
                Ok(path.to_path_buf())
            } else {
                Ok(std::env::current_dir()?.join(path))
            }
        }
    }
}

fn convert_iso_to_xiso(input: &Path, output: &Path) -> anyhow::Result<()> {
    let input_meta = std::fs::metadata(input)
        .with_context(|| format!("Failed to read input metadata: {}", input.display()))?;
    if !input_meta.is_file() {
        bail!("Input path is not a file: {}", input.display());
    }

    if output.exists() {
        let in_abs = canonical_or_absolute(input)
            .with_context(|| format!("Failed to resolve input path: {}", input.display()))?;
        let out_abs = canonical_or_absolute(output)
            .with_context(|| format!("Failed to resolve output path: {}", output.display()))?;
        if in_abs == out_abs {
            bail!("Input and output paths are the same file");
        }
    }

    if let Some(parent) = output.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent).with_context(|| {
                format!("Failed to create output parent directory: {}", parent.display())
            })?;
        }
    }

    let source_file = File::open(input)
        .with_context(|| format!("Failed to open input file: {}", input.display()))?;
    let source = BufReader::new(source_file);
    let source = OffsetWrapper::new(source)
        .with_context(|| format!("Input is not a valid Xbox ISO/XISO image: {}", input.display()))?;

    let mut fs = XDVDFSFilesystem::<_, _, StdIOCopier<_, _>>::new(source)
        .ok_or_else(|| anyhow!("Failed to mount XDVDFS source image"))?;

    let output_file = File::options()
        .write(true)
        .truncate(true)
        .create(true)
        .open(output)
        .with_context(|| format!("Failed to open output file: {}", output.display()))?;
    let mut output_writer = BufWriter::with_capacity(1024 * 1024, output_file);

    create_xdvdfs_image(&mut fs, &mut output_writer, NoOpProgressVisitor)
        .context("Failed while creating XISO image")?;
    output_writer
        .flush()
        .context("Failed to flush output image writer")?;

    Ok(())
}

fn write_err_buf(err_buf: *mut c_char, err_buf_len: usize, msg: &str) {
    if err_buf.is_null() || err_buf_len == 0 {
        return;
    }

    // SAFETY: caller owns writable `err_buf` with length `err_buf_len`.
    let out = unsafe { std::slice::from_raw_parts_mut(err_buf as *mut u8, err_buf_len) };
    out.fill(0);

    let bytes = msg.as_bytes();
    let copy_len = bytes.len().min(err_buf_len.saturating_sub(1));
    out[..copy_len].copy_from_slice(&bytes[..copy_len]);
}

fn c_path_to_owned(ptr: *const c_char, arg_name: &str) -> anyhow::Result<String> {
    if ptr.is_null() {
        bail!("{arg_name} was null");
    }

    // SAFETY: pointer validity/null termination is guaranteed by caller contract.
    let c_str = unsafe { CStr::from_ptr(ptr) };
    let s = c_str
        .to_str()
        .with_context(|| format!("{arg_name} was not valid UTF-8"))?;
    Ok(s.to_owned())
}

#[no_mangle]
pub extern "C" fn xiso_convert_iso_to_xiso(
    input_path: *const c_char,
    output_path: *const c_char,
    err_buf: *mut c_char,
    err_buf_len: usize,
) -> i32 {
    let outcome = std::panic::catch_unwind(|| {
        let input = c_path_to_owned(input_path, "input_path")?;
        let output = c_path_to_owned(output_path, "output_path")?;
        convert_iso_to_xiso(Path::new(&input), Path::new(&output))
    });

    match outcome {
        Ok(Ok(())) => {
            write_err_buf(err_buf, err_buf_len, "");
            0
        }
        Ok(Err(e)) => {
            write_err_buf(err_buf, err_buf_len, &e.to_string());
            1
        }
        Err(_) => {
            write_err_buf(err_buf, err_buf_len, "ISO conversion panicked");
            2
        }
    }
}
