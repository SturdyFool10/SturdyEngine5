//! Whole-file helpers and directories, and the generic byte-stream layer: copy between any reader and writer, buffering, and framing.

use crate::net::{TcpStream, UdpSocket};
use crate::sync::Stream;
use crate::system::{Child, File};
use crate::{bytes_ptr, check, flag, run, Lower, Object, Result};
use crate::ffi;

fn one_object(begin: impl FnOnce(*mut ffi::SturdyAsyncObject) -> i32) -> Result<Object> {
    let mut raw = ffi::SturdyAsyncObject::default();
    check(begin(&mut raw))?;
    Ok(Object::from_raw(raw))
}

// ------------------------------------------------------------------------------------------------------------------------------
// Whole-file helpers
// ------------------------------------------------------------------------------------------------------------------------------

/// Whole-file helpers and directory operations, run on the IO lane.
pub mod fs {
    use super::*;

    /// Reads a whole file; fails with `InvalidArgument` when it is larger than `max_bytes` (0: unlimited).
    pub async fn read(path: &str, max_bytes: u64) -> Result<Vec<u8>> {
        let mut cx = Lower::default();
        let path = cx.string(path);
        Ok(run(|out| unsafe { ffi::sturdy_fs_read_file(path, max_bytes, out) }).await?.bytes())
    }

    /// Reads `byte_count` bytes at `offset` (fewer at the end of the file).
    pub async fn read_range(path: &str, offset: u64, byte_count: u64) -> Result<Vec<u8>> {
        let mut cx = Lower::default();
        let path = cx.string(path);
        Ok(run(|out| unsafe { ffi::sturdy_fs_read_range(path, offset, byte_count, out) }).await?.bytes())
    }

    /// Creates or replaces the file with `data`.
    pub async fn write(path: &str, data: &[u8]) -> Result<()> {
        write_file(path, data, false).await
    }

    /// Appends `data` to the file (creating it if needed).
    pub async fn append(path: &str, data: &[u8]) -> Result<()> {
        write_file(path, data, true).await
    }

    async fn write_file(path: &str, data: &[u8], append: bool) -> Result<()> {
        let mut cx = Lower::default();
        let path = cx.string(path);
        let (ptr, len) = bytes_ptr(data);
        run(|out| unsafe { ffi::sturdy_fs_write_file(path, ptr, len, flag(append), out) }).await.map(|_| ())
    }

    pub async fn size(path: &str) -> Result<u64> {
        let mut cx = Lower::default();
        let path = cx.string(path);
        Ok(run(|out| unsafe { ffi::sturdy_fs_size(path, out) }).await?.number() as u64)
    }

    pub async fn exists(path: &str) -> Result<bool> {
        let mut cx = Lower::default();
        let path = cx.string(path);
        Ok(run(|out| unsafe { ffi::sturdy_fs_exists(path, out) }).await?.number() == 1)
    }

    pub async fn create_directories(path: &str) -> Result<()> {
        let mut cx = Lower::default();
        let path = cx.string(path);
        run(|out| unsafe { ffi::sturdy_fs_create_directories(path, out) }).await.map(|_| ())
    }

    /// True if something was removed.
    pub async fn remove_file(path: &str) -> Result<bool> {
        let mut cx = Lower::default();
        let path = cx.string(path);
        Ok(run(|out| unsafe { ffi::sturdy_fs_remove_file(path, out) }).await?.number() == 1)
    }

    /// Removes a directory and everything in it. True if something was removed.
    pub async fn remove_directory(path: &str) -> Result<bool> {
        let mut cx = Lower::default();
        let path = cx.string(path);
        Ok(run(|out| unsafe { ffi::sturdy_fs_remove_directory(path, out) }).await?.number() == 1)
    }

    pub async fn copy(source: &str, destination: &str) -> Result<()> {
        let mut cx = Lower::default();
        let (source, destination) = (cx.string(source), cx.string(destination));
        run(|out| unsafe { ffi::sturdy_fs_copy(source, destination, out) }).await.map(|_| ())
    }

    pub async fn rename(source: &str, destination: &str) -> Result<()> {
        let mut cx = Lower::default();
        let (source, destination) = (cx.string(source), cx.string(destination));
        run(|out| unsafe { ffi::sturdy_fs_rename(source, destination, out) }).await.map(|_| ())
    }

    /// Entry paths of a directory (at most `max_entries`, 0: 4096).
    pub async fn list(path: &str, max_entries: u64, recursive: bool) -> Result<Vec<String>> {
        let mut cx = Lower::default();
        let path = cx.string(path);
        let reply = run(|out| unsafe { ffi::sturdy_fs_list(path, max_entries, flag(recursive), out) }).await?;
        Ok(crate::unpack_frames(&reply.bytes())?.into_iter().map(|f| String::from_utf8_lossy(&f).into_owned()).collect())
    }

    /// Walks a directory entry by entry.
    pub struct DirIter {
        object: Object,
    }

    impl DirIter {
        pub fn new(path: &str, recursive: bool) -> Result<DirIter> {
            let mut cx = Lower::default();
            let path = cx.string(path);
            Ok(DirIter { object: one_object(|out| unsafe { ffi::sturdy_dir_iter_create(path, flag(recursive), out) })? })
        }

        /// The next path, or `None` at the end.
        pub async fn next(&self) -> Result<Option<String>> {
            let reply = run(|out| unsafe { ffi::sturdy_dir_iter_next(self.object.raw(), out) }).await?;
            Ok((reply.number() == 1).then(|| reply.text()))
        }
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Generic byte streams
// ------------------------------------------------------------------------------------------------------------------------------

/// Which pipe of a child process a [`ByteStream`] means.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Pipe {
    Stdout,
    Stderr,
    Stdin,
}

impl Pipe {
    fn raw(self) -> i32 {
        match self {
            Pipe::Stdout => ffi::STURDY_PROCESS_STDOUT,
            Pipe::Stderr => ffi::STURDY_PROCESS_STDERR,
            Pipe::Stdin => ffi::STURDY_PROCESS_STDIN,
        }
    }
}

/// Something bytes can be read from or written to by [`copy`], [`read_to_end`], [`write_all`], [`BufferedReader`], [`BufferedWriter`] and
/// [`Framed`]: a TCP stream, a file, a child's pipes ([`ChildExt`]), an in-memory pipe end, a [`MemoryReader`] or a [`MemoryWriter`].
pub trait ByteStream {
    #[doc(hidden)]
    fn io(&self) -> (ffi::SturdyAsyncObject, i32);
}

impl ByteStream for TcpStream {
    fn io(&self) -> (ffi::SturdyAsyncObject, i32) {
        (self.object.raw(), Pipe::Stdout.raw())
    }
}

impl ByteStream for File {
    fn io(&self) -> (ffi::SturdyAsyncObject, i32) {
        (self.object.raw(), Pipe::Stdout.raw())
    }
}

impl<T: ByteStream + ?Sized> ByteStream for &T {
    fn io(&self) -> (ffi::SturdyAsyncObject, i32) {
        (**self).io()
    }
}

/// A child's stdout, stderr or stdin as a [`ByteStream`].
pub struct ChildPipe<'a> {
    child: &'a Child,
    pipe: Pipe,
}

impl ByteStream for ChildPipe<'_> {
    fn io(&self) -> (ffi::SturdyAsyncObject, i32) {
        (self.child.object.raw(), self.pipe.raw())
    }
}

/// Adds the child's pipes to the generic stream layer.
pub trait ChildExt {
    fn stdout(&self) -> ChildPipe<'_>;
    fn stderr(&self) -> ChildPipe<'_>;
    fn stdin(&self) -> ChildPipe<'_>;
}

impl ChildExt for Child {
    fn stdout(&self) -> ChildPipe<'_> {
        ChildPipe { child: self, pipe: Pipe::Stdout }
    }
    fn stderr(&self) -> ChildPipe<'_> {
        ChildPipe { child: self, pipe: Pipe::Stderr }
    }
    fn stdin(&self) -> ChildPipe<'_> {
        ChildPipe { child: self, pipe: Pipe::Stdin }
    }
}

/// A reader over a copy of some bytes.
pub struct MemoryReader {
    object: Object,
}

impl MemoryReader {
    pub fn new(data: &[u8]) -> Result<MemoryReader> {
        let (ptr, len) = bytes_ptr(data);
        Ok(MemoryReader { object: one_object(|out| unsafe { ffi::sturdy_memory_reader_create(ptr, len, out) })? })
    }
}

impl ByteStream for MemoryReader {
    fn io(&self) -> (ffi::SturdyAsyncObject, i32) {
        (self.object.raw(), Pipe::Stdout.raw())
    }
}

/// A writer that collects what it is given.
pub struct MemoryWriter {
    object: Object,
}

impl MemoryWriter {
    pub fn new() -> Result<MemoryWriter> {
        Ok(MemoryWriter { object: one_object(|out| unsafe { ffi::sturdy_memory_writer_create(out) })? })
    }

    /// What has been written so far.
    pub fn bytes(&self) -> Result<Vec<u8>> {
        let mut length = 0u64;
        check(unsafe { ffi::sturdy_memory_writer_bytes(self.object.raw(), core::ptr::null_mut(), 0, &mut length) })?;
        let mut buffer = vec![0u8; length as usize];
        if !buffer.is_empty() {
            check(unsafe { ffi::sturdy_memory_writer_bytes(self.object.raw(), buffer.as_mut_ptr(), buffer.len() as u64, &mut length) })?;
        }
        Ok(buffer)
    }
}

impl ByteStream for MemoryWriter {
    fn io(&self) -> (ffi::SturdyAsyncObject, i32) {
        (self.object.raw(), Pipe::Stdout.raw())
    }
}

/// One end of an in-memory pipe: what one end writes the other reads.
pub struct PipeEnd {
    object: Object,
}

impl ByteStream for PipeEnd {
    fn io(&self) -> (ffi::SturdyAsyncObject, i32) {
        (self.object.raw(), Pipe::Stdout.raw())
    }
}

/// Two connected in-memory ends. `chunks`: messages buffered per direction (0: 8).
pub fn memory_pipe(chunks: u64) -> Result<(PipeEnd, PipeEnd)> {
    let (mut a, mut b) = (ffi::SturdyAsyncObject::default(), ffi::SturdyAsyncObject::default());
    check(unsafe { ffi::sturdy_memory_pipe_create(chunks, &mut a, &mut b) })?;
    Ok((PipeEnd { object: Object::from_raw(a) }, PipeEnd { object: Object::from_raw(b) }))
}

/// Options for [`copy`].
#[derive(Debug, Clone, Copy, Default)]
pub struct CopyOptions {
    /// 0: 64 KiB.
    pub chunk_bytes: u64,
    /// Stop after this many bytes; 0: until the reader ends.
    pub limit: u64,
    /// Shut the writer down (close a TCP write side, sync a file, close a process's stdin) when done.
    pub shutdown_writer: bool,
}

/// Copies until the reader ends, overlapping each read with the previous write. Returns the bytes copied.
pub async fn copy(reader: &impl ByteStream, writer: &impl ByteStream, options: &CopyOptions) -> Result<u64> {
    let (r, rs) = reader.io();
    let (w, ws) = writer.io();
    let lowered = ffi::SturdyCopyOptions { chunk_bytes: options.chunk_bytes, limit: options.limit, shutdown_writer: flag(options.shutdown_writer) };
    Ok(run(|out| unsafe { ffi::sturdy_io_copy(r, rs, w, ws, &lowered, out) }).await?.number() as u64)
}

/// Everything until the reader ends; `LimitExceeded` past `max_bytes` (0: unlimited).
pub async fn read_to_end(reader: &impl ByteStream, max_bytes: u64) -> Result<Vec<u8>> {
    let (r, rs) = reader.io();
    Ok(run(|out| unsafe { ffi::sturdy_io_read_to_end(r, rs, max_bytes, out) }).await?.bytes())
}

pub async fn write_all(writer: &impl ByteStream, data: &[u8]) -> Result<()> {
    let (w, ws) = writer.io();
    let (ptr, len) = bytes_ptr(data);
    run(|out| unsafe { ffi::sturdy_io_write_all(w, ws, ptr, len, out) }).await.map(|_| ())
}

/// Adds read-ahead to a reader: lines, delimiters, exact counts. One operation at a time.
pub struct BufferedReader {
    object: Object,
}

impl BufferedReader {
    /// `chunk_bytes` 0: 16 KiB.
    pub fn new(reader: &impl ByteStream, chunk_bytes: u64) -> Result<BufferedReader> {
        let (r, rs) = reader.io();
        Ok(BufferedReader { object: one_object(|out| unsafe { ffi::sturdy_buffered_reader_create(r, rs, chunk_bytes, out) })? })
    }

    /// Up to `max_bytes` (empty at the end).
    pub async fn read_some(&self, max_bytes: u64) -> Result<Vec<u8>> {
        Ok(run(|out| unsafe { ffi::sturdy_buffered_read_some(self.object.raw(), max_bytes, out) }).await?.bytes())
    }

    /// Exactly `count` bytes; `UnexpectedEof` when the stream ends first.
    pub async fn read_exact(&self, count: u64) -> Result<Vec<u8>> {
        Ok(run(|out| unsafe { ffi::sturdy_buffered_read_exact(self.object.raw(), count, out) }).await?.bytes())
    }

    /// Up to and including `delimiter`; `None` at a clean end of stream.
    pub async fn read_until(&self, delimiter: &str, max_bytes: u64) -> Result<Option<Vec<u8>>> {
        let mut cx = Lower::default();
        let delimiter = cx.string(delimiter);
        let reply = run(|out| unsafe { ffi::sturdy_buffered_read_until(self.object.raw(), delimiter, max_bytes, out) }).await?;
        Ok((reply.number() == 1).then(|| reply.bytes()))
    }

    /// One line without its terminator (LF or CRLF); `None` at a clean end of stream. `max_bytes` 0: 64 KiB.
    pub async fn read_line(&self, max_bytes: u64) -> Result<Option<String>> {
        let reply = run(|out| unsafe { ffi::sturdy_buffered_read_line(self.object.raw(), max_bytes, out) }).await?;
        Ok((reply.number() == 1).then(|| reply.text()))
    }

    /// Bytes read ahead and not yet handed out.
    pub fn buffered(&self) -> u64 {
        let mut bytes = 0u64;
        unsafe { ffi::sturdy_buffered_reader_buffered(self.object.raw(), &mut bytes) };
        bytes
    }
}

/// Batches small writes; call [`BufferedWriter::flush`] (or [`BufferedWriter::shutdown`]) to send what is pending.
pub struct BufferedWriter {
    object: Object,
}

impl BufferedWriter {
    /// `capacity` 0: 16 KiB.
    pub fn new(writer: &impl ByteStream, capacity: u64) -> Result<BufferedWriter> {
        let (w, ws) = writer.io();
        Ok(BufferedWriter { object: one_object(|out| unsafe { ffi::sturdy_buffered_writer_create(w, ws, capacity, out) })? })
    }

    pub async fn write(&self, data: &[u8]) -> Result<()> {
        let (ptr, len) = bytes_ptr(data);
        run(|out| unsafe { ffi::sturdy_buffered_write(self.object.raw(), ptr, len, out) }).await.map(|_| ())
    }

    pub async fn flush(&self) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_buffered_flush(self.object.raw(), out) }).await.map(|_| ())
    }

    /// Flushes, then shuts the underlying writer down.
    pub async fn shutdown(&self) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_buffered_shutdown(self.object.raw(), out) }).await.map(|_| ())
    }
}

/// How a [`Framed`] stream delimits messages.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrameCodec {
    /// A 4-byte big-endian length prefix, then the frame.
    LengthDelimited,
    /// One line per frame (no newline inside a frame).
    Lines,
}

/// Messages over a byte stream that reads and writes (a TCP stream, a file, a pipe end).
pub struct Framed {
    object: Object,
}

impl Framed {
    /// `max_frame_bytes` 0: 8 MiB (64 KiB for lines).
    pub fn new(io: &impl ByteStream, codec: FrameCodec, max_frame_bytes: u64) -> Result<Framed> {
        let (object, _) = io.io();
        let codec = match codec {
            FrameCodec::LengthDelimited => ffi::STURDY_FRAME_LENGTH_DELIMITED,
            FrameCodec::Lines => ffi::STURDY_FRAME_LINES,
        };
        Ok(Framed { object: one_object(|out| unsafe { ffi::sturdy_framed_create(object, codec, max_frame_bytes, out) })? })
    }

    /// The next frame, or `None` at a clean end of stream.
    pub async fn next(&self) -> Result<Option<Vec<u8>>> {
        let reply = run(|out| unsafe { ffi::sturdy_framed_next(self.object.raw(), out) }).await?;
        Ok((reply.number() == 1).then(|| reply.bytes()))
    }

    pub async fn send(&self, frame: &[u8]) -> Result<()> {
        let (ptr, len) = bytes_ptr(frame);
        run(|out| unsafe { ffi::sturdy_framed_send(self.object.raw(), ptr, len, out) }).await.map(|_| ())
    }

    /// The remaining frames as a [`Stream`] (do not call [`Framed::next`] meanwhile).
    pub fn into_stream(&self) -> Result<Stream> {
        Ok(Stream::from_object(one_object(|out| unsafe { ffi::sturdy_framed_stream(self.object.raw(), out) })?))
    }
}

#[allow(dead_code)]
fn _udp_is_not_a_byte_stream(_: &UdpSocket) {}
