use async_trait::async_trait;
use nfsserve::nfs::{
    fattr3, fileid3, filename3, ftype3, nfspath3, nfsstat3, nfstime3, sattr3, set_atime, set_gid3,
    set_mode3, set_mtime, set_size3, set_uid3, specdata3,
};
use nfsserve::tcp::{NFSTcp, NFSTcpListener};
use nfsserve::vfs::{DirEntry, NFSFileSystem, ReadDirResult, VFSCapabilities};
use serde::Deserialize;
use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};

const RUNTIME_ABI_VERSION: u32 = 1;
const RUNTIME_EXCLUSIVE_GATEWAY: u32 = 2;
const XATTR_ALWAYS_SET: c_int = 0;
const XATTR_DELETE: c_int = 3;
const TIME_ACCESS: u32 = 1;
const TIME_MODIFY: u32 = 2;
const APPLEDOUBLE_XATTR: &str = "io.vexdb.macos.appledouble";
// NFSv3 can only expose macOS xattrs as visible `._name` files. Keep the
// default Bash workspace clean and report mounted xattrs as unsupported;
// native VexFS xattr APIs remain available through the database/runtime.
const APPLEDOUBLE_SIDECARS_ENABLED: bool = false;
const SIDECAR_INODE_START: u64 = u64::MAX - 1024;
const METADATA_CACHE_LIMIT: usize = 100_000;
const METADATA_CACHE_TTL_MS: i64 = 250;
const DEFERRED_PUBLISH_IDLE_MS: i64 = 500;
const DEFERRED_PUBLISH_MAX_AGE_MS: i64 = 30_000;
const DEFERRED_PUBLISH_DIRTY_BYTES: u64 = 4 * 1024 * 1024;
const DEFERRED_PUBLISH_BATCH_BYTES: u64 = 8 * 1024 * 1024;
const DEFERRED_PUBLISH_FILE_THRESHOLD: usize = 1_024;
const DEFERRED_PUBLISH_RETRY_INITIAL_MS: i64 = 250;
const DEFERRED_PUBLISH_RETRY_MAX_MS: i64 = 5_000;
const MOUNT_SESSION_KEEPALIVE_INTERVAL_MS: u64 = 5_000;
// Remote PostgreSQL may spend tens of milliseconds publishing one version.
// Keep one background lock hold below the macOS NFS retransmit window; a large
// batch makes unrelated foreground reads look like a dead server.
const DEFERRED_PUBLISH_BATCH_SIZE: i64 = 8;

fn deferred_publish_retry_delay_ms(consecutive_failures: u32) -> i64 {
    let shift = consecutive_failures.saturating_sub(1).min(5);
    DEFERRED_PUBLISH_RETRY_INITIAL_MS
        .saturating_mul(1_i64 << shift)
        .min(DEFERRED_PUBLISH_RETRY_MAX_MS)
}

#[repr(C)]
pub struct VexfsNfsGatewayConfig {
    abi_version: u32,
    backend: *const c_char,
    connection: *const c_char,
    workspace: *const c_char,
    principal: *const c_char,
    listen_address: *const c_char,
    port: u16,
    operation_timeout_ms: u32,
}

#[repr(C)]
struct RuntimeConfig {
    abi_version: u32,
    backend: *const c_char,
    connection: *const c_char,
    workspace: *const c_char,
    principal: *const c_char,
    operation_timeout_ms: u32,
    flags: u32,
}

#[repr(C)]
struct RuntimeBytes {
    data: *mut c_void,
    size: u64,
}

#[repr(C)]
struct RuntimeError {
    status: c_int,
    native_code: c_int,
    backend: [c_char; 16],
    message: [c_char; 512],
}

#[repr(C)]
struct RuntimeVisibility {
    workspace_head: i64,
    cache_generation: u64,
    external_commit: c_int,
}

enum RuntimeSessionOpaque {}
type RuntimeStatus = c_int;

extern "C" {
    fn vexfs_mount_session_open(
        config: *const RuntimeConfig,
        session: *mut *mut RuntimeSessionOpaque,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_session_keepalive(
        session: *mut RuntimeSessionOpaque,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_session_close(session: *mut RuntimeSessionOpaque);
    fn vexfs_mount_stat(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        json: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_path_for_inode(
        session: *mut RuntimeSessionOpaque,
        inode: i64,
        path: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_list(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        json: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_refresh_visibility(
        session: *mut RuntimeSessionOpaque,
        visibility: *mut RuntimeVisibility,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_read_file_range(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        offset: u64,
        length: u64,
        content: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_mkdir(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_set_mode(
        session: *mut RuntimeSessionOpaque,
        inode: i64,
        mode: u32,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_set_times(
        session: *mut RuntimeSessionOpaque,
        inode: i64,
        accessed_at_ms: i64,
        modified_at_ms: i64,
        mask: u32,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_chown(
        session: *mut RuntimeSessionOpaque,
        inode: i64,
        uid: i64,
        gid: i64,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_symlink(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        target: *const c_void,
        target_size: u64,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_readlink(
        session: *mut RuntimeSessionOpaque,
        inode: i64,
        target: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_rename(
        session: *mut RuntimeSessionOpaque,
        source: *const c_char,
        destination: *const c_char,
        replace: c_int,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_link(
        session: *mut RuntimeSessionOpaque,
        source: *const c_char,
        destination: *const c_char,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_remove(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        recursive: c_int,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_xattr_get(
        session: *mut RuntimeSessionOpaque,
        inode: i64,
        name: *const c_char,
        value: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_xattr_set(
        session: *mut RuntimeSessionOpaque,
        inode: i64,
        name: *const c_char,
        value: *const c_void,
        size: u64,
        policy: c_int,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_handle_open(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        flags: *const c_char,
        request_id: *const c_char,
        handle: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_handle_create_owned_stat_durable(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        mode: u32,
        uid: i64,
        gid: i64,
        request_id: *const c_char,
        durability: *const c_char,
        handle: *mut RuntimeBytes,
        stat_json: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_handle_stage_write_durable(
        session: *mut RuntimeSessionOpaque,
        handle: *const c_char,
        offset: u64,
        data: *const c_void,
        size: u64,
        request_id: *const c_char,
        durability: *const c_char,
        generation: *mut i64,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_handle_truncate(
        session: *mut RuntimeSessionOpaque,
        handle: *const c_char,
        size: u64,
        request_id: *const c_char,
        generation: *mut i64,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_handle_read(
        session: *mut RuntimeSessionOpaque,
        handle: *const c_char,
        offset: u64,
        length: u64,
        content: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_handle_publish_close(
        session: *mut RuntimeSessionOpaque,
        handle: *const c_char,
        generation: i64,
        durability: *const c_char,
        version: *mut i64,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_handle_publish_close_background(
        session: *mut RuntimeSessionOpaque,
        handle: *const c_char,
        generation: i64,
        durability: *const c_char,
        version: *mut i64,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_handle_close(
        session: *mut RuntimeSessionOpaque,
        handle: *const c_char,
        retain_unpublished: c_int,
        request_id: *const c_char,
        state: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_synchronize(
        session: *mut RuntimeSessionOpaque,
        request_id: *const c_char,
        published: *mut i64,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_publish_close_batch(
        session: *mut RuntimeSessionOpaque,
        durability: *const c_char,
        max_count: i64,
        published: *mut i64,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_free(memory: *mut c_void);
}

#[derive(Clone, Deserialize)]
struct FileStat {
    inode: i64,
    kind: String,
    mode: u32,
    uid: i64,
    gid: i64,
    size: i64,
    link_count: i64,
    accessed_at: i64,
    updated_at: i64,
    changed_at: i64,
}

#[derive(Clone, Deserialize)]
struct ListEntry {
    name: String,
    inode: i64,
    kind: String,
    mode: u32,
    uid: i64,
    gid: i64,
    size: i64,
    link_count: i64,
    accessed_at: i64,
    updated_at: i64,
    changed_at: i64,
}

impl From<ListEntry> for FileStat {
    fn from(entry: ListEntry) -> Self {
        Self {
            inode: entry.inode,
            kind: entry.kind,
            mode: entry.mode,
            uid: entry.uid,
            gid: entry.gid,
            size: entry.size,
            link_count: entry.link_count,
            accessed_at: entry.accessed_at,
            updated_at: entry.updated_at,
            changed_at: entry.changed_at,
        }
    }
}

#[derive(Clone, Hash, PartialEq, Eq)]
struct SidecarKey {
    parent: fileid3,
    name: Vec<u8>,
}

struct Sidecar {
    key: SidecarKey,
    target_inode: Option<fileid3>,
    data: Vec<u8>,
    mode: u32,
    updated_at_ms: i64,
}

struct PendingWrite {
    handle: CString,
    generation: i64,
    logical_size: u64,
    updated_at_ms: i64,
    first_dirty_at_ms: i64,
    dirty_bytes: u64,
    publishing_generation: Option<i64>,
}

#[derive(Clone)]
struct PublishCandidate {
    inode: fileid3,
    handle: CString,
    generation: i64,
    logical_size: u64,
}

#[derive(Deserialize)]
struct PublishedClaim {
    handle: String,
    generation: i64,
    #[allow(dead_code)]
    version: i64,
}

struct PublishBatchOutcome {
    published: Vec<PublishedClaim>,
    failure: Option<nfsstat3>,
}

struct RuntimeState {
    session: *mut RuntimeSessionOpaque,
    postgresql_backend: bool,
    root_inode: fileid3,
    uid: u32,
    gid: u32,
    strict_durability: bool,
    next_sidecar_inode: fileid3,
    sidecars_by_key: HashMap<SidecarKey, fileid3>,
    sidecars: HashMap<fileid3, Sidecar>,
    pending_writes: HashMap<fileid3, PendingWrite>,
    paths: HashMap<fileid3, String>,
    inodes_by_path: HashMap<String, fileid3>,
    stats: HashMap<fileid3, FileStat>,
    directory_entries: HashMap<fileid3, HashMap<Vec<u8>, fileid3>>,
    metadata_cache_checked_at_ms: i64,
}

unsafe impl Send for RuntimeState {}

impl Drop for RuntimeState {
    fn drop(&mut self) {
        if !self.session.is_null() {
            unsafe { vexfs_mount_session_close(self.session) };
            self.session = ptr::null_mut();
        }
    }
}

pub struct VexfsNfs {
    state: Arc<Mutex<RuntimeState>>,
    request_prefix: String,
    request_sequence: AtomicU64,
}

fn empty_error() -> RuntimeError {
    RuntimeError {
        status: 0,
        native_code: 0,
        backend: [0; 16],
        message: [0; 512],
    }
}

fn runtime_to_nfs(status: RuntimeStatus) -> nfsstat3 {
    match status {
        1 => nfsstat3::NFS3ERR_INVAL,
        2 => nfsstat3::NFS3ERR_NOENT,
        3 => nfsstat3::NFS3ERR_EXIST,
        4 => nfsstat3::NFS3ERR_ROFS,
        5 => nfsstat3::NFS3ERR_JUKEBOX,
        8 => nfsstat3::NFS3ERR_ACCES,
        9 => nfsstat3::NFS3ERR_NOSPC,
        11 => nfsstat3::NFS3ERR_NOTSUPP,
        12 => nfsstat3::NFS3ERR_NOTEMPTY,
        _ => nfsstat3::NFS3ERR_IO,
    }
}

fn checked_cstring(value: &str) -> Result<CString, nfsstat3> {
    CString::new(value).map_err(|_| nfsstat3::NFS3ERR_INVAL)
}

fn bytes_to_string(bytes: RuntimeBytes) -> Result<String, nfsstat3> {
    let value = unsafe {
        if bytes.data.is_null() {
            Vec::new()
        } else {
            let value =
                std::slice::from_raw_parts(bytes.data.cast::<u8>(), bytes.size as usize).to_vec();
            vexfs_mount_free(bytes.data);
            value
        }
    };
    String::from_utf8(value).map_err(|_| nfsstat3::NFS3ERR_IO)
}

fn take_bytes(bytes: RuntimeBytes) -> Vec<u8> {
    unsafe {
        if bytes.data.is_null() {
            Vec::new()
        } else {
            let value =
                std::slice::from_raw_parts(bytes.data.cast::<u8>(), bytes.size as usize).to_vec();
            vexfs_mount_free(bytes.data);
            value
        }
    }
}

fn runtime_result(status: RuntimeStatus) -> Result<(), nfsstat3> {
    if status == 0 {
        Ok(())
    } else {
        Err(runtime_to_nfs(status))
    }
}

fn publish_claimed_batch(
    session: *mut RuntimeSessionOpaque,
    candidates: &[PublishCandidate],
) -> PublishBatchOutcome {
    let durability = CString::new("full").unwrap();
    let mut published = Vec::with_capacity(candidates.len());
    for candidate in candidates {
        let Ok(handle) = candidate.handle.to_str() else {
            return PublishBatchOutcome {
                published,
                failure: Some(nfsstat3::NFS3ERR_IO),
            };
        };
        let mut version = 0;
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_handle_publish_close_background(
                session,
                candidate.handle.as_ptr(),
                candidate.generation,
                durability.as_ptr(),
                &mut version,
                &mut error,
            )
        };
        // Each call is its own implicit PG transaction. If a later call fails,
        // the gateway clears completed items and releases only the unreported
        // remainder. An unknown commit result is still safe to retry because
        // the exact generation returns its committed version idempotently.
        if let Err(failure) = runtime_result(status) {
            return PublishBatchOutcome {
                published,
                failure: Some(failure),
            };
        }
        published.push(PublishedClaim {
            handle: handle.to_owned(),
            generation: candidate.generation,
            version,
        });
    }
    PublishBatchOutcome {
        published,
        failure: None,
    }
}

fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis()
        .min(i64::MAX as u128) as i64
}

fn nfs_time(milliseconds: i64) -> nfstime3 {
    let value = milliseconds.max(0) as u64;
    nfstime3 {
        seconds: (value / 1000).min(u32::MAX as u64) as u32,
        nseconds: ((value % 1000) * 1_000_000) as u32,
    }
}

fn stat_attr(stat: &FileStat) -> fattr3 {
    let ftype = match stat.kind.as_str() {
        "directory" => ftype3::NF3DIR,
        "symlink" => ftype3::NF3LNK,
        _ => ftype3::NF3REG,
    };
    fattr3 {
        ftype,
        mode: stat.mode & 0o7777,
        nlink: stat.link_count.max(1).min(u32::MAX as i64) as u32,
        uid: stat.uid.max(0).min(u32::MAX as i64) as u32,
        gid: stat.gid.max(0).min(u32::MAX as i64) as u32,
        size: stat.size.max(0) as u64,
        used: stat.size.max(0) as u64,
        rdev: specdata3::default(),
        fsid: 1,
        fileid: stat.inode as u64,
        atime: nfs_time(stat.accessed_at),
        mtime: nfs_time(stat.updated_at),
        ctime: nfs_time(stat.changed_at),
    }
}

impl RuntimeState {
    fn refresh_metadata_cache_if_stale(&mut self) -> Result<(), nfsstat3> {
        let now = now_ms();
        if now.saturating_sub(self.metadata_cache_checked_at_ms) < METADATA_CACHE_TTL_MS {
            return Ok(());
        }
        let mut visibility = RuntimeVisibility {
            workspace_head: 0,
            cache_generation: 0,
            external_commit: 0,
        };
        let mut error = empty_error();
        runtime_result(unsafe {
            vexfs_mount_refresh_visibility(self.session, &mut visibility, &mut error)
        })?;
        // Local create/write/publish paths update their cached entries directly.
        // Only another database connection can make those entries stale. Paths
        // stay available so already-issued NFS file handles remain usable.
        if visibility.external_commit != 0 {
            self.stats.clear();
            self.directory_entries.clear();
        }
        self.metadata_cache_checked_at_ms = now;
        Ok(())
    }

    fn clear_metadata_cache(&mut self) {
        self.paths.clear();
        self.inodes_by_path.clear();
        self.stats.clear();
        self.directory_entries.clear();
    }

    fn cache_stat(&mut self, path: &str, stat: &FileStat) {
        if self.stats.len() >= METADATA_CACHE_LIMIT {
            self.clear_metadata_cache();
        }
        let inode = stat.inode as u64;
        self.paths.insert(inode, path.to_string());
        self.inodes_by_path.insert(path.to_string(), inode);
        self.stats.insert(inode, stat.clone());
    }

    fn stat_path(&mut self, path: &str) -> Result<FileStat, nfsstat3> {
        self.refresh_metadata_cache_if_stale()?;
        if let Some(inode) = self.inodes_by_path.get(path) {
            if let Some(stat) = self.stats.get(inode) {
                return Ok(stat.clone());
            }
        }
        let path_text = path.to_string();
        let path = checked_cstring(path)?;
        let mut bytes = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status =
            unsafe { vexfs_mount_stat(self.session, path.as_ptr(), &mut bytes, &mut error) };
        runtime_result(status)?;
        let stat: FileStat =
            serde_json::from_str(&bytes_to_string(bytes)?).map_err(|_| nfsstat3::NFS3ERR_IO)?;
        self.cache_stat(&path_text, &stat);
        Ok(stat)
    }

    fn path_for_inode(&mut self, inode: fileid3) -> Result<String, nfsstat3> {
        if self.sidecars.contains_key(&inode) {
            return Err(nfsstat3::NFS3ERR_INVAL);
        }
        if let Some(path) = self.paths.get(&inode) {
            return Ok(path.clone());
        }
        let mut bytes = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_path_for_inode(self.session, inode as i64, &mut bytes, &mut error)
        };
        runtime_result(status)?;
        let path = bytes_to_string(bytes)?;
        self.paths.insert(inode, path.clone());
        self.inodes_by_path.insert(path.clone(), inode);
        Ok(path)
    }

    fn child_path(&mut self, parent: fileid3, name: &[u8]) -> Result<String, nfsstat3> {
        if name.is_empty() || name.contains(&b'/') || name.contains(&0) {
            return Err(nfsstat3::NFS3ERR_INVAL);
        }
        let name = std::str::from_utf8(name).map_err(|_| nfsstat3::NFS3ERR_INVAL)?;
        let parent = self.path_for_inode(parent)?;
        Ok(if parent == "/" {
            format!("/{name}")
        } else {
            format!("{parent}/{name}")
        })
    }

    fn list(&mut self, path: &str) -> Result<Vec<ListEntry>, nfsstat3> {
        let path_text = path.to_string();
        let path = checked_cstring(path)?;
        let mut bytes = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status =
            unsafe { vexfs_mount_list(self.session, path.as_ptr(), &mut bytes, &mut error) };
        runtime_result(status)?;
        let entries: Vec<ListEntry> =
            serde_json::from_str(&bytes_to_string(bytes)?).map_err(|_| nfsstat3::NFS3ERR_IO)?;
        if let Ok(parent) = self.stat_path(&path_text) {
            let parent_inode = parent.inode as u64;
            let mut cached = HashMap::new();
            for entry in &entries {
                let inode = entry.inode as u64;
                cached.insert(entry.name.as_bytes().to_vec(), inode);
                let child_path = if path_text == "/" {
                    format!("/{}", entry.name)
                } else {
                    format!("{path_text}/{}", entry.name)
                };
                let stat: FileStat = entry.clone().into();
                self.cache_stat(&child_path, &stat);
            }
            self.directory_entries.insert(parent_inode, cached);
        }
        Ok(entries)
    }

    fn read_file_range(
        &mut self,
        path: &str,
        offset: u64,
        length: u64,
    ) -> Result<Vec<u8>, nfsstat3> {
        let path = checked_cstring(path)?;
        let mut bytes = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_read_file_range(
                self.session,
                path.as_ptr(),
                offset,
                length,
                &mut bytes,
                &mut error,
            )
        };
        runtime_result(status)?;
        Ok(take_bytes(bytes))
    }

    fn read_inode_range(
        &mut self,
        inode: fileid3,
        path: &str,
        offset: u64,
        length: u64,
    ) -> Result<Vec<u8>, nfsstat3> {
        let Some(pending) = self.pending_writes.get(&inode) else {
            return self.read_file_range(path, offset, length);
        };
        if pending.publishing_generation.is_some() {
            return Err(nfsstat3::NFS3ERR_JUKEBOX);
        }
        let handle = pending.handle.clone();
        let mut bytes = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_handle_read(
                self.session,
                handle.as_ptr(),
                offset,
                length,
                &mut bytes,
                &mut error,
            )
        };
        runtime_result(status)?;
        Ok(take_bytes(bytes))
    }

    fn request_id(prefix: &str, sequence: &AtomicU64, operation: &str) -> CString {
        CString::new(format!(
            "{prefix}-{operation}-{}",
            sequence.fetch_add(1, Ordering::Relaxed)
        ))
        .expect("request id has no NUL")
    }

    fn create_file(
        &mut self,
        path: &str,
        mode: u32,
        prefix: &str,
        sequence: &AtomicU64,
    ) -> Result<(CString, FileStat), nfsstat3> {
        let path = checked_cstring(path)?;
        let request = Self::request_id(prefix, sequence, "create");
        let durability = CString::new(if self.strict_durability {
            "full"
        } else {
            "none"
        })
        .unwrap();
        let mut handle = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut stat_json = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_handle_create_owned_stat_durable(
                self.session,
                path.as_ptr(),
                mode & 0o7777,
                self.uid as i64,
                self.gid as i64,
                request.as_ptr(),
                durability.as_ptr(),
                &mut handle,
                &mut stat_json,
                &mut error,
            )
        };
        runtime_result(status)?;
        let handle = checked_cstring(&bytes_to_string(handle)?)?;
        let stat =
            serde_json::from_str(&bytes_to_string(stat_json)?).map_err(|_| nfsstat3::NFS3ERR_IO)?;
        Ok((handle, stat))
    }

    fn stage_file_write(
        &mut self,
        inode: fileid3,
        path: &str,
        offset: u64,
        data: &[u8],
        prefix: &str,
        sequence: &AtomicU64,
    ) -> Result<(), nfsstat3> {
        if self
            .pending_writes
            .get(&inode)
            .is_some_and(|pending| pending.publishing_generation.is_some())
        {
            return Err(nfsstat3::NFS3ERR_JUKEBOX);
        }
        if !self.pending_writes.contains_key(&inode) {
            let path = checked_cstring(path)?;
            let mode = CString::new("rw").unwrap();
            let request = Self::request_id(prefix, sequence, "open-staged-write");
            let mut handle = RuntimeBytes {
                data: ptr::null_mut(),
                size: 0,
            };
            let mut error = empty_error();
            let status = unsafe {
                vexfs_mount_handle_open(
                    self.session,
                    path.as_ptr(),
                    mode.as_ptr(),
                    request.as_ptr(),
                    &mut handle,
                    &mut error,
                )
            };
            runtime_result(status)?;
            let logical_size = self
                .stat_path(std::str::from_utf8(path.as_bytes()).unwrap())?
                .size
                .max(0) as u64;
            let dirty_at = now_ms();
            self.pending_writes.insert(
                inode,
                PendingWrite {
                    handle: checked_cstring(&bytes_to_string(handle)?)?,
                    generation: 0,
                    logical_size,
                    updated_at_ms: dirty_at,
                    first_dirty_at_ms: dirty_at,
                    dirty_bytes: 0,
                    publishing_generation: None,
                },
            );
        }
        let handle = self.pending_writes.get(&inode).unwrap().handle.clone();
        let request = Self::request_id(prefix, sequence, "stage-write");
        // NFS UNSTABLE writes become durable at COMMIT.  Making every incoming
        // block FULL turns a sequential file into thousands of fsync calls.
        let durability = CString::new("none").unwrap();
        let mut generation = 0;
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_handle_stage_write_durable(
                self.session,
                handle.as_ptr(),
                offset,
                data.as_ptr().cast(),
                data.len() as u64,
                request.as_ptr(),
                durability.as_ptr(),
                &mut generation,
                &mut error,
            )
        };
        runtime_result(status)?;
        let pending = self.pending_writes.get_mut(&inode).unwrap();
        pending.generation = generation;
        pending.logical_size = pending
            .logical_size
            .max(offset.saturating_add(data.len() as u64));
        pending.dirty_bytes = pending.dirty_bytes.saturating_add(data.len() as u64);
        let updated_at_ms = now_ms();
        pending.updated_at_ms = updated_at_ms;
        let logical_size = pending.logical_size;
        if let Some(stat) = self.stats.get_mut(&inode) {
            stat.size = logical_size.min(i64::MAX as u64) as i64;
            stat.updated_at = updated_at_ms;
            stat.changed_at = updated_at_ms;
        }
        Ok(())
    }

    fn publish_pending(&mut self, inode: fileid3, durability: &str) -> Result<bool, nfsstat3> {
        let Some(pending) = self.pending_writes.get(&inode) else {
            return Ok(false);
        };
        if pending.publishing_generation.is_some() {
            return Err(nfsstat3::NFS3ERR_JUKEBOX);
        }
        let handle = pending.handle.clone();
        let generation = pending.generation;
        let logical_size = pending.logical_size;
        let durability = CString::new(durability).unwrap();
        let mut version = 0;
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_handle_publish_close(
                self.session,
                handle.as_ptr(),
                generation,
                durability.as_ptr(),
                &mut version,
                &mut error,
            )
        };
        runtime_result(status)?;
        self.pending_writes.remove(&inode);
        if let Some(stat) = self.stats.get_mut(&inode) {
            stat.size = logical_size.min(i64::MAX as u64) as i64;
            stat.updated_at = now_ms();
            stat.changed_at = stat.updated_at;
        }
        Ok(true)
    }

    fn publish_pending_batch(
        &mut self,
        durability: &str,
        max_count: i64,
    ) -> Result<usize, nfsstat3> {
        if self.pending_writes.is_empty() || max_count <= 0 {
            return Ok(0);
        }
        let mut handles: Vec<(fileid3, Vec<u8>, u64)> = self
            .pending_writes
            .iter()
            .map(|(inode, pending)| {
                (
                    *inode,
                    pending.handle.as_bytes().to_vec(),
                    pending.logical_size,
                )
            })
            .collect();
        handles.sort_by(|left, right| left.1.cmp(&right.1));
        handles.truncate(max_count as usize);
        let durability = CString::new(durability).unwrap();
        let mut published = 0;
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_publish_close_batch(
                self.session,
                durability.as_ptr(),
                max_count,
                &mut published,
                &mut error,
            )
        };
        runtime_result(status)?;
        let published = published.max(0) as usize;
        if published > handles.len() {
            return Err(nfsstat3::NFS3ERR_IO);
        }
        let now = now_ms();
        for (inode, _, logical_size) in handles.into_iter().take(published) {
            self.pending_writes.remove(&inode);
            if let Some(stat) = self.stats.get_mut(&inode) {
                stat.size = logical_size.min(i64::MAX as u64) as i64;
                stat.updated_at = now;
                stat.changed_at = now;
            }
        }
        Ok(published)
    }

    fn claim_deferred_publish_batch(&mut self, now: i64) -> Vec<PublishCandidate> {
        let available: Vec<fileid3> = self
            .pending_writes
            .iter()
            .filter(|(_, pending)| pending.publishing_generation.is_none())
            .map(|(inode, _)| *inode)
            .collect();
        if available.is_empty() {
            return Vec::new();
        }
        let dirty_bytes = available.iter().fold(0_u64, |total, inode| {
            total.saturating_add(
                self.pending_writes
                    .get(inode)
                    .map(|pending| pending.dirty_bytes.max(1))
                    .unwrap_or(0),
            )
        });
        let oldest_age = available
            .iter()
            .filter_map(|inode| self.pending_writes.get(inode))
            .map(|pending| now.saturating_sub(pending.first_dirty_at_ms))
            .max()
            .unwrap_or(0);
        let latest_update = available
            .iter()
            .filter_map(|inode| self.pending_writes.get(inode))
            .map(|pending| pending.updated_at_ms)
            .max()
            .unwrap_or(now);
        let globally_idle = now.saturating_sub(latest_update) >= DEFERRED_PUBLISH_IDLE_MS;
        let force = available.len() >= DEFERRED_PUBLISH_FILE_THRESHOLD
            || dirty_bytes >= DEFERRED_PUBLISH_DIRTY_BYTES
            || oldest_age >= DEFERRED_PUBLISH_MAX_AGE_MS;
        if !globally_idle && !force {
            return Vec::new();
        }

        let mut ordered = available;
        ordered.sort_by(|left, right| {
            let left_pending = self.pending_writes.get(left).unwrap();
            let right_pending = self.pending_writes.get(right).unwrap();
            left_pending
                .updated_at_ms
                .cmp(&right_pending.updated_at_ms)
                .then_with(|| {
                    left_pending
                        .handle
                        .as_bytes()
                        .cmp(right_pending.handle.as_bytes())
                })
        });

        let mut claimed = Vec::new();
        let mut claimed_bytes = 0_u64;
        for inode in ordered {
            if claimed.len() >= DEFERRED_PUBLISH_BATCH_SIZE as usize {
                break;
            }
            let pending = self.pending_writes.get(&inode).unwrap();
            // Thresholds wake the publisher during a long foreground burst,
            // but they must never close the handle currently receiving NFS
            // WRITE calls.  Older idle files may be drained concurrently; the
            // active file is published by COMMIT/fsync or after it becomes idle.
            if !globally_idle
                && now.saturating_sub(pending.updated_at_ms) < DEFERRED_PUBLISH_IDLE_MS
            {
                continue;
            }
            let candidate_bytes = pending.dirty_bytes.max(1);
            if !claimed.is_empty()
                && claimed_bytes.saturating_add(candidate_bytes) > DEFERRED_PUBLISH_BATCH_BYTES
            {
                break;
            }
            claimed_bytes = claimed_bytes.saturating_add(candidate_bytes);
            claimed.push(PublishCandidate {
                inode,
                handle: pending.handle.clone(),
                generation: pending.generation,
                logical_size: pending.logical_size,
            });
        }
        for candidate in &claimed {
            if let Some(pending) = self.pending_writes.get_mut(&candidate.inode) {
                if pending.generation == candidate.generation {
                    pending.publishing_generation = Some(candidate.generation);
                }
            }
        }
        claimed
    }

    fn release_publish_claims(&mut self, candidates: &[PublishCandidate]) {
        for candidate in candidates {
            if let Some(pending) = self.pending_writes.get_mut(&candidate.inode) {
                if pending.publishing_generation == Some(candidate.generation) {
                    pending.publishing_generation = None;
                }
            }
        }
    }

    fn complete_publish_claims(
        &mut self,
        candidates: &[PublishCandidate],
        published: &[PublishedClaim],
    ) -> Result<usize, nfsstat3> {
        if published.len() > candidates.len()
            || !candidates
                .iter()
                .zip(published.iter())
                .all(|(candidate, item)| {
                    candidate.handle.to_bytes() == item.handle.as_bytes()
                        && candidate.generation == item.generation
                })
        {
            self.release_publish_claims(candidates);
            return Err(nfsstat3::NFS3ERR_IO);
        }
        let updated_at = now_ms();
        let mut completed = 0;
        for (index, candidate) in candidates.iter().enumerate() {
            let matches = self
                .pending_writes
                .get(&candidate.inode)
                .is_some_and(|pending| {
                    pending.handle.as_bytes() == candidate.handle.as_bytes()
                        && pending.generation == candidate.generation
                        && pending.publishing_generation == Some(candidate.generation)
                });
            if !matches {
                continue;
            }
            if index < published.len() {
                self.pending_writes.remove(&candidate.inode);
                if let Some(stat) = self.stats.get_mut(&candidate.inode) {
                    stat.size = candidate.logical_size.min(i64::MAX as u64) as i64;
                    stat.updated_at = updated_at;
                    stat.changed_at = updated_at;
                }
                completed += 1;
            } else if let Some(pending) = self.pending_writes.get_mut(&candidate.inode) {
                pending.publishing_generation = None;
            }
        }
        Ok(completed)
    }

    fn truncate_file(
        &mut self,
        inode: fileid3,
        path: &str,
        size: u64,
        prefix: &str,
        sequence: &AtomicU64,
    ) -> Result<(), nfsstat3> {
        if let Some(pending) = self.pending_writes.get(&inode) {
            if pending.publishing_generation.is_some() {
                return Err(nfsstat3::NFS3ERR_JUKEBOX);
            }
            let handle = pending.handle.clone();
            let request = Self::request_id(prefix, sequence, "truncate-pending");
            let mut generation = 0;
            let mut error = empty_error();
            let status = unsafe {
                vexfs_mount_handle_truncate(
                    self.session,
                    handle.as_ptr(),
                    size,
                    request.as_ptr(),
                    &mut generation,
                    &mut error,
                )
            };
            runtime_result(status)?;
            let pending = self.pending_writes.get_mut(&inode).unwrap();
            pending.generation = generation;
            pending.logical_size = size;
            pending.updated_at_ms = now_ms();
            pending.dirty_bytes = pending.dirty_bytes.saturating_add(1);
            self.publish_pending(inode, "full")?;
            return Ok(());
        }
        let path = checked_cstring(path)?;
        let mode = CString::new("rw").unwrap();
        let open_request = Self::request_id(prefix, sequence, "open-truncate");
        let mut handle = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_handle_open(
                self.session,
                path.as_ptr(),
                mode.as_ptr(),
                open_request.as_ptr(),
                &mut handle,
                &mut error,
            )
        };
        runtime_result(status)?;
        let handle = checked_cstring(&bytes_to_string(handle)?)?;
        let request = Self::request_id(prefix, sequence, "truncate");
        let mut generation = 0;
        let status = unsafe {
            vexfs_mount_handle_truncate(
                self.session,
                handle.as_ptr(),
                size,
                request.as_ptr(),
                &mut generation,
                &mut error,
            )
        };
        if status != 0 {
            self.close_failed_handle(&handle, prefix, sequence);
            return Err(runtime_to_nfs(status));
        }
        let durability = CString::new("full").unwrap();
        let mut version = 0;
        let status = unsafe {
            vexfs_mount_handle_publish_close(
                self.session,
                handle.as_ptr(),
                generation,
                durability.as_ptr(),
                &mut version,
                &mut error,
            )
        };
        if status != 0 {
            self.close_failed_handle(&handle, prefix, sequence);
        }
        runtime_result(status)
    }

    fn close_failed_handle(&mut self, handle: &CString, prefix: &str, sequence: &AtomicU64) {
        let request = Self::request_id(prefix, sequence, "close-failed");
        let mut state = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        unsafe {
            vexfs_mount_handle_close(
                self.session,
                handle.as_ptr(),
                1,
                request.as_ptr(),
                &mut state,
                &mut error,
            );
        }
        let _ = take_bytes(state);
    }

    fn target_name(sidecar_name: &[u8]) -> Option<&[u8]> {
        sidecar_name
            .strip_prefix(b"._")
            .filter(|name| !name.is_empty() && *name != b"." && *name != b"..")
    }

    fn sidecar_target(
        &mut self,
        parent: fileid3,
        sidecar_name: &[u8],
    ) -> Result<(String, fileid3), nfsstat3> {
        let target_name = Self::target_name(sidecar_name).ok_or(nfsstat3::NFS3ERR_NOENT)?;
        let path = self.child_path(parent, target_name)?;
        let stat = self.stat_path(&path)?;
        Ok((path, stat.inode as u64))
    }

    fn get_xattr(&mut self, inode: fileid3, name: &str) -> Result<Vec<u8>, nfsstat3> {
        let name = checked_cstring(name)?;
        let mut bytes = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_xattr_get(
                self.session,
                inode as i64,
                name.as_ptr(),
                &mut bytes,
                &mut error,
            )
        };
        runtime_result(status)?;
        Ok(take_bytes(bytes))
    }

    fn set_xattr(
        &mut self,
        inode: fileid3,
        name: &str,
        data: &[u8],
        policy: c_int,
    ) -> Result<(), nfsstat3> {
        let name = checked_cstring(name)?;
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_xattr_set(
                self.session,
                inode as i64,
                name.as_ptr(),
                data.as_ptr().cast(),
                data.len() as u64,
                policy,
                &mut error,
            )
        };
        runtime_result(status)
    }

    fn allocate_sidecar(
        &mut self,
        key: SidecarKey,
        target_inode: Option<fileid3>,
        data: Vec<u8>,
        mode: u32,
    ) -> fileid3 {
        if let Some(inode) = self.sidecars_by_key.get(&key) {
            return *inode;
        }
        let inode = self.next_sidecar_inode;
        self.next_sidecar_inode = self.next_sidecar_inode.saturating_sub(1);
        self.sidecars_by_key.insert(key.clone(), inode);
        self.sidecars.insert(
            inode,
            Sidecar {
                key,
                target_inode,
                data,
                mode,
                updated_at_ms: now_ms(),
            },
        );
        inode
    }

    fn lookup_sidecar(&mut self, parent: fileid3, name: &[u8]) -> Result<fileid3, nfsstat3> {
        let key = SidecarKey {
            parent,
            name: name.to_vec(),
        };
        if let Some(inode) = self.sidecars_by_key.get(&key) {
            return Ok(*inode);
        }
        let (_, target_inode) = self.sidecar_target(parent, name)?;
        let data = self.get_xattr(target_inode, APPLEDOUBLE_XATTR)?;
        Ok(self.allocate_sidecar(key, Some(target_inode), data, 0o600))
    }

    fn sidecar_attr(&self, inode: fileid3) -> Result<fattr3, nfsstat3> {
        let sidecar = self.sidecars.get(&inode).ok_or(nfsstat3::NFS3ERR_STALE)?;
        Ok(fattr3 {
            ftype: ftype3::NF3REG,
            mode: sidecar.mode,
            nlink: 1,
            uid: self.uid,
            gid: self.gid,
            size: sidecar.data.len() as u64,
            used: sidecar.data.len() as u64,
            rdev: specdata3::default(),
            fsid: 1,
            fileid: inode,
            atime: nfs_time(sidecar.updated_at_ms),
            mtime: nfs_time(sidecar.updated_at_ms),
            ctime: nfs_time(sidecar.updated_at_ms),
        })
    }

    fn persist_sidecar(&mut self, inode: fileid3) -> Result<(), nfsstat3> {
        let (key, target_inode, data) = {
            let sidecar = self.sidecars.get(&inode).ok_or(nfsstat3::NFS3ERR_STALE)?;
            (
                sidecar.key.clone(),
                sidecar.target_inode,
                sidecar.data.clone(),
            )
        };
        let target_inode = match target_inode {
            Some(value) => value,
            None => self.sidecar_target(key.parent, &key.name)?.1,
        };
        self.set_xattr(target_inode, APPLEDOUBLE_XATTR, &data, XATTR_ALWAYS_SET)?;
        if let Some(sidecar) = self.sidecars.get_mut(&inode) {
            sidecar.target_inode = Some(target_inode);
        }
        Ok(())
    }

    fn remove_sidecar(&mut self, parent: fileid3, name: &[u8]) -> Result<(), nfsstat3> {
        let key = SidecarKey {
            parent,
            name: name.to_vec(),
        };
        let inode = self.sidecars_by_key.remove(&key);
        let target_inode = inode
            .and_then(|value| self.sidecars.remove(&value))
            .and_then(|value| value.target_inode)
            .or_else(|| self.sidecar_target(parent, name).ok().map(|value| value.1));
        let Some(target_inode) = target_inode else {
            return Err(nfsstat3::NFS3ERR_NOENT);
        };
        match self.set_xattr(target_inode, APPLEDOUBLE_XATTR, &[], XATTR_DELETE) {
            Ok(()) | Err(nfsstat3::NFS3ERR_NOENT) => Ok(()),
            Err(status) => Err(status),
        }
    }
}

impl VexfsNfs {
    fn request_id(&self, operation: &str) -> CString {
        RuntimeState::request_id(&self.request_prefix, &self.request_sequence, operation)
    }
}

#[async_trait]
impl NFSFileSystem for VexfsNfs {
    fn capabilities(&self) -> VFSCapabilities {
        VFSCapabilities::ReadWrite
    }

    fn root_dir(&self) -> fileid3 {
        self.state.lock().expect("runtime mutex").root_inode
    }

    async fn lookup(&self, dirid: fileid3, filename: &filename3) -> Result<fileid3, nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        state.refresh_metadata_cache_if_stale()?;
        if filename.as_ref() == b"." {
            return Ok(dirid);
        }
        if filename.as_ref() == b".." {
            let path = state.path_for_inode(dirid)?;
            if path == "/" {
                return Ok(dirid);
            }
            let parent = path.rsplit_once('/').map(|value| value.0).unwrap_or("");
            let parent = if parent.is_empty() { "/" } else { parent };
            return Ok(state.stat_path(parent)?.inode as u64);
        }
        if RuntimeState::target_name(filename.as_ref()).is_some() {
            if !APPLEDOUBLE_SIDECARS_ENABLED {
                return Err(nfsstat3::NFS3ERR_NOENT);
            }
            return state.lookup_sidecar(dirid, filename.as_ref());
        }
        if let Some(entries) = state.directory_entries.get(&dirid) {
            return entries
                .get(filename.as_ref())
                .copied()
                .ok_or(nfsstat3::NFS3ERR_NOENT);
        }
        let path = state.child_path(dirid, filename.as_ref())?;
        Ok(state.stat_path(&path)?.inode as u64)
    }

    async fn getattr(&self, id: fileid3) -> Result<fattr3, nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        if state.sidecars.contains_key(&id) {
            return state.sidecar_attr(id);
        }
        let path = state.path_for_inode(id)?;
        let mut attr = stat_attr(&state.stat_path(&path)?);
        if let Some(pending) = state.pending_writes.get(&id) {
            attr.size = pending.logical_size;
            attr.used = pending.logical_size;
        }
        Ok(attr)
    }

    async fn setattr(&self, id: fileid3, attrs: sattr3) -> Result<fattr3, nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        if state.sidecars.contains_key(&id) {
            if let set_mode3::mode(mode) = attrs.mode {
                state.sidecars.get_mut(&id).unwrap().mode = mode & 0o7777;
            }
            if let set_size3::size(size) = attrs.size {
                state
                    .sidecars
                    .get_mut(&id)
                    .unwrap()
                    .data
                    .resize(size as usize, 0);
                state.persist_sidecar(id)?;
            }
            return state.sidecar_attr(id);
        }
        let path = state.path_for_inode(id)?;
        let mut error = empty_error();
        if let set_mode3::mode(mode) = attrs.mode {
            runtime_result(unsafe {
                vexfs_mount_set_mode(state.session, id as i64, mode & 0o7777, &mut error)
            })?;
        }
        let uid = match attrs.uid {
            set_uid3::uid(value) => value as i64,
            set_uid3::Void => -1,
        };
        let gid = match attrs.gid {
            set_gid3::gid(value) => value as i64,
            set_gid3::Void => -1,
        };
        if uid >= 0 || gid >= 0 {
            runtime_result(unsafe {
                vexfs_mount_chown(state.session, id as i64, uid, gid, &mut error)
            })?;
        }
        if let set_size3::size(size) = attrs.size {
            state.truncate_file(
                id,
                &path,
                size,
                &self.request_prefix,
                &self.request_sequence,
            )?;
        }
        let now = now_ms();
        let (accessed, access_mask) = match attrs.atime {
            set_atime::DONT_CHANGE => (0, 0),
            set_atime::SET_TO_SERVER_TIME => (now, TIME_ACCESS),
            set_atime::SET_TO_CLIENT_TIME(value) => (
                (value.seconds as i64) * 1000 + (value.nseconds as i64) / 1_000_000,
                TIME_ACCESS,
            ),
        };
        let (modified, modify_mask) = match attrs.mtime {
            set_mtime::DONT_CHANGE => (0, 0),
            set_mtime::SET_TO_SERVER_TIME => (now, TIME_MODIFY),
            set_mtime::SET_TO_CLIENT_TIME(value) => (
                (value.seconds as i64) * 1000 + (value.nseconds as i64) / 1_000_000,
                TIME_MODIFY,
            ),
        };
        if access_mask | modify_mask != 0 {
            runtime_result(unsafe {
                vexfs_mount_set_times(
                    state.session,
                    id as i64,
                    accessed,
                    modified,
                    access_mask | modify_mask,
                    &mut error,
                )
            })?;
        }
        state.stats.remove(&id);
        Ok(stat_attr(&state.stat_path(&path)?))
    }

    async fn read(
        &self,
        id: fileid3,
        offset: u64,
        count: u32,
    ) -> Result<(Vec<u8>, bool), nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        if let Some(sidecar) = state.sidecars.get(&id) {
            let start = (offset as usize).min(sidecar.data.len());
            let end = start.saturating_add(count as usize).min(sidecar.data.len());
            return Ok((sidecar.data[start..end].to_vec(), end == sidecar.data.len()));
        }
        let path = state.path_for_inode(id)?;
        let stat = state.stat_path(&path)?;
        let data = state.read_inode_range(id, &path, offset, count as u64)?;
        let size = state
            .pending_writes
            .get(&id)
            .map(|pending| pending.logical_size)
            .unwrap_or(stat.size.max(0) as u64);
        Ok((data, offset.saturating_add(count as u64) >= size))
    }

    async fn write(
        &self,
        id: fileid3,
        offset: u64,
        data: &[u8],
        stable_requested: bool,
    ) -> Result<(fattr3, bool), nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        if state.sidecars.contains_key(&id) {
            let sidecar = state.sidecars.get_mut(&id).unwrap();
            let offset = offset as usize;
            if offset.saturating_add(data.len()) > sidecar.data.len() {
                sidecar.data.resize(offset.saturating_add(data.len()), 0);
            }
            sidecar.data[offset..offset + data.len()].copy_from_slice(data);
            sidecar.updated_at_ms = now_ms();
            state.persist_sidecar(id)?;
            if state.strict_durability && stable_requested {
                let request = self.request_id("nfs-sidecar-write");
                let mut published = 0;
                let mut error = empty_error();
                runtime_result(unsafe {
                    vexfs_mount_synchronize(
                        state.session,
                        request.as_ptr(),
                        &mut published,
                        &mut error,
                    )
                })?;
            }
            return Ok((
                state.sidecar_attr(id)?,
                !state.strict_durability || stable_requested,
            ));
        }
        let path = state.path_for_inode(id)?;
        state.stage_file_write(
            id,
            &path,
            offset,
            data,
            &self.request_prefix,
            &self.request_sequence,
        )?;
        if state.strict_durability && stable_requested {
            state.publish_pending(id, "full")?;
            return Ok((stat_attr(&state.stat_path(&path)?), true));
        }
        let mut attr = stat_attr(&state.stat_path(&path)?);
        if let Some(pending) = state.pending_writes.get(&id) {
            attr.size = pending.logical_size;
            attr.used = pending.logical_size;
        }
        // Balanced mode coalesces small files in database-backed staging, but
        // once one handle has accumulated enough write traffic it must make
        // the NFS client issue COMMIT at fsync/close. Otherwise a large active
        // file can sit behind thousands of older idle files in the background
        // queue and still be unpublished when the gateway is stopped.
        let requires_foreground_commit = state
            .pending_writes
            .get(&id)
            .is_some_and(|pending| pending.dirty_bytes >= DEFERRED_PUBLISH_DIRTY_BYTES);
        // Strict mode reports UNSTABLE unless this request was actually FULL.
        Ok((
            attr,
            !state.strict_durability && !requires_foreground_commit,
        ))
    }

    async fn create(
        &self,
        dirid: fileid3,
        filename: &filename3,
        attrs: sattr3,
    ) -> Result<(fileid3, fattr3), nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let mode = match attrs.mode {
            set_mode3::mode(value) => value,
            set_mode3::Void => 0o644,
        };
        if RuntimeState::target_name(filename.as_ref()).is_some() {
            if !APPLEDOUBLE_SIDECARS_ENABLED {
                return Err(nfsstat3::NFS3ERR_NOTSUPP);
            }
            let key = SidecarKey {
                parent: dirid,
                name: filename.as_ref().to_vec(),
            };
            let target_inode = state
                .sidecar_target(dirid, filename.as_ref())
                .ok()
                .map(|value| value.1);
            let inode = state.allocate_sidecar(key, target_inode, Vec::new(), mode);
            return Ok((inode, state.sidecar_attr(inode)?));
        }
        let path = state.child_path(dirid, filename.as_ref())?;
        let (handle, stat) =
            state.create_file(&path, mode, &self.request_prefix, &self.request_sequence)?;
        state.cache_stat(&path, &stat);
        let dirty_at = now_ms();
        state.pending_writes.insert(
            stat.inode as u64,
            PendingWrite {
                handle,
                generation: 1,
                logical_size: 0,
                updated_at_ms: dirty_at,
                first_dirty_at_ms: dirty_at,
                dirty_bytes: 0,
                publishing_generation: None,
            },
        );
        if let Some(entries) = state.directory_entries.get_mut(&dirid) {
            entries.insert(filename.as_ref().to_vec(), stat.inode as u64);
        }
        state.stats.remove(&dirid);
        Ok((stat.inode as u64, stat_attr(&stat)))
    }

    async fn create_exclusive(
        &self,
        dirid: fileid3,
        filename: &filename3,
    ) -> Result<fileid3, nfsstat3> {
        Ok(self.create(dirid, filename, sattr3::default()).await?.0)
    }

    async fn mkdir(
        &self,
        dirid: fileid3,
        dirname: &filename3,
    ) -> Result<(fileid3, fattr3), nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let path = state.child_path(dirid, dirname.as_ref())?;
        let path_c = checked_cstring(&path)?;
        let mut error = empty_error();
        runtime_result(unsafe { vexfs_mount_mkdir(state.session, path_c.as_ptr(), &mut error) })?;
        let stat = state.stat_path(&path)?;
        let mode = 0o755;
        runtime_result(unsafe {
            vexfs_mount_set_mode(state.session, stat.inode, mode & 0o7777, &mut error)
        })?;
        runtime_result(unsafe {
            vexfs_mount_chown(
                state.session,
                stat.inode,
                state.uid as i64,
                state.gid as i64,
                &mut error,
            )
        })?;
        state.stats.remove(&(stat.inode as u64));
        let stat = state.stat_path(&path)?;
        if let Some(entries) = state.directory_entries.get_mut(&dirid) {
            entries.insert(dirname.as_ref().to_vec(), stat.inode as u64);
        }
        state
            .directory_entries
            .insert(stat.inode as u64, HashMap::new());
        state.stats.remove(&dirid);
        Ok((stat.inode as u64, stat_attr(&stat)))
    }

    async fn remove(&self, dirid: fileid3, filename: &filename3) -> Result<(), nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        if RuntimeState::target_name(filename.as_ref()).is_some() {
            if !APPLEDOUBLE_SIDECARS_ENABLED {
                return Err(nfsstat3::NFS3ERR_NOENT);
            }
            return state.remove_sidecar(dirid, filename.as_ref());
        }
        let path = state.child_path(dirid, filename.as_ref())?;
        let inode = state.stat_path(&path).ok().map(|stat| stat.inode as u64);
        if let Some(inode) = inode {
            state.publish_pending(inode, "none")?;
        }
        let path_c = checked_cstring(&path)?;
        let mut error = empty_error();
        runtime_result(unsafe {
            vexfs_mount_remove(state.session, path_c.as_ptr(), 0, &mut error)
        })?;
        if let Some(inode) = inode {
            state.paths.remove(&inode);
            state.stats.remove(&inode);
            state.directory_entries.remove(&inode);
        }
        state.inodes_by_path.remove(&path);
        if let Some(entries) = state.directory_entries.get_mut(&dirid) {
            entries.remove(filename.as_ref());
        }
        state.stats.remove(&dirid);
        Ok(())
    }

    async fn rename(
        &self,
        from_dirid: fileid3,
        from_filename: &filename3,
        to_dirid: fileid3,
        to_filename: &filename3,
    ) -> Result<(), nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let from_sidecar = RuntimeState::target_name(from_filename.as_ref()).is_some();
        let to_sidecar = RuntimeState::target_name(to_filename.as_ref()).is_some();
        if from_sidecar || to_sidecar {
            if !APPLEDOUBLE_SIDECARS_ENABLED {
                return Err(nfsstat3::NFS3ERR_NOTSUPP);
            }
            if !from_sidecar || !to_sidecar {
                return Err(nfsstat3::NFS3ERR_INVAL);
            }
            let inode = state.lookup_sidecar(from_dirid, from_filename.as_ref())?;
            let data = state.sidecars.get(&inode).unwrap().data.clone();
            state.remove_sidecar(from_dirid, from_filename.as_ref())?;
            let key = SidecarKey {
                parent: to_dirid,
                name: to_filename.as_ref().to_vec(),
            };
            let target = state
                .sidecar_target(to_dirid, to_filename.as_ref())
                .ok()
                .map(|value| value.1);
            let new_inode = state.allocate_sidecar(key, target, data, 0o600);
            return state.persist_sidecar(new_inode);
        }
        let source_path = state.child_path(from_dirid, from_filename.as_ref())?;
        let destination_path = state.child_path(to_dirid, to_filename.as_ref())?;
        if let Ok(stat) = state.stat_path(&source_path) {
            state.publish_pending(stat.inode as u64, "none")?;
        }
        if let Ok(stat) = state.stat_path(&destination_path) {
            state.publish_pending(stat.inode as u64, "none")?;
        }
        let source = checked_cstring(&source_path)?;
        let destination = checked_cstring(&destination_path)?;
        let mut error = empty_error();
        runtime_result(unsafe {
            vexfs_mount_rename(
                state.session,
                source.as_ptr(),
                destination.as_ptr(),
                1,
                &mut error,
            )
        })?;
        state.clear_metadata_cache();
        Ok(())
    }

    async fn link(
        &self,
        id: fileid3,
        dirid: fileid3,
        filename: &filename3,
    ) -> Result<fattr3, nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        if state.sidecars.contains_key(&id)
            || RuntimeState::target_name(filename.as_ref()).is_some()
        {
            return Err(nfsstat3::NFS3ERR_NOTSUPP);
        }
        state.publish_pending(id, "none")?;
        let source_path = state.path_for_inode(id)?;
        let destination_path = state.child_path(dirid, filename.as_ref())?;
        let source = checked_cstring(&source_path)?;
        let destination = checked_cstring(&destination_path)?;
        let mut error = empty_error();
        runtime_result(unsafe {
            vexfs_mount_link(
                state.session,
                source.as_ptr(),
                destination.as_ptr(),
                &mut error,
            )
        })?;
        state.inodes_by_path.insert(destination_path.clone(), id);
        if let Some(entries) = state.directory_entries.get_mut(&dirid) {
            entries.insert(filename.as_ref().to_vec(), id);
        }
        state.stats.remove(&id);
        state.stats.remove(&dirid);
        Ok(stat_attr(&state.stat_path(&source_path)?))
    }

    async fn commit(&self, id: fileid3, _offset: u64, _count: u32) -> Result<fattr3, nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        if state.sidecars.contains_key(&id) {
            state.persist_sidecar(id)?;
            return state.sidecar_attr(id);
        }
        if state.publish_pending(id, "full")? {
            let path = state.path_for_inode(id)?;
            return Ok(stat_attr(&state.stat_path(&path)?));
        }
        // NFS COMMIT applies only to this file/range. macOS may emit a final
        // COMMIT for an inode that is already clean while unmounting. Treating
        // that as a workspace-wide synchronize publishes unrelated handles
        // without closing them and leaves stale staging metadata behind.
        let path = state.path_for_inode(id)?;
        Ok(stat_attr(&state.stat_path(&path)?))
    }

    async fn readdir(
        &self,
        dirid: fileid3,
        start_after: fileid3,
        max_entries: usize,
    ) -> Result<ReadDirResult, nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let path = state.path_for_inode(dirid)?;
        let mut entries = state.list(&path)?;
        entries.sort_by_key(|entry| entry.inode);
        let start = if start_after == 0 {
            0
        } else {
            entries
                .iter()
                .position(|entry| entry.inode as u64 == start_after)
                .map(|value| value + 1)
                .unwrap_or(0)
        };
        let end = start.saturating_add(max_entries).min(entries.len());
        let complete = end == entries.len();
        let pending_sizes: HashMap<fileid3, u64> = state
            .pending_writes
            .iter()
            .map(|(inode, pending)| (*inode, pending.logical_size))
            .collect();
        let entries = entries[start..end]
            .iter()
            .map(|entry| {
                let mut stat: FileStat = entry.clone().into();
                if let Some(size) = pending_sizes.get(&(entry.inode as u64)) {
                    stat.size = (*size).min(i64::MAX as u64) as i64;
                }
                DirEntry {
                    fileid: entry.inode as u64,
                    name: entry.name.as_bytes().into(),
                    attr: stat_attr(&stat),
                }
            })
            .collect();
        Ok(ReadDirResult {
            entries,
            end: complete,
        })
    }

    async fn symlink(
        &self,
        dirid: fileid3,
        linkname: &filename3,
        target: &nfspath3,
        _attrs: &sattr3,
    ) -> Result<(fileid3, fattr3), nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let path_text = state.child_path(dirid, linkname.as_ref())?;
        let path = checked_cstring(&path_text)?;
        let mut error = empty_error();
        runtime_result(unsafe {
            vexfs_mount_symlink(
                state.session,
                path.as_ptr(),
                target.as_ref().as_ptr().cast(),
                target.as_ref().len() as u64,
                &mut error,
            )
        })?;
        let stat = state.stat_path(&path_text)?;
        if let Some(entries) = state.directory_entries.get_mut(&dirid) {
            entries.insert(linkname.as_ref().to_vec(), stat.inode as u64);
        }
        state.stats.remove(&dirid);
        Ok((stat.inode as u64, stat_attr(&stat)))
    }

    async fn readlink(&self, id: fileid3) -> Result<nfspath3, nfsstat3> {
        let state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let mut bytes = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status =
            unsafe { vexfs_mount_readlink(state.session, id as i64, &mut bytes, &mut error) };
        runtime_result(status)?;
        Ok(take_bytes(bytes).into())
    }
}

fn config_string(pointer: *const c_char, name: &str) -> Result<String, String> {
    if pointer.is_null() {
        return Err(format!("{name} is required"));
    }
    unsafe { CStr::from_ptr(pointer) }
        .to_str()
        .map(str::to_owned)
        .map_err(|_| format!("{name} is not UTF-8"))
}

fn write_error(buffer: *mut c_char, size: usize, message: &str) {
    if buffer.is_null() || size == 0 {
        return;
    }
    let bytes = message.as_bytes();
    let count = bytes.len().min(size - 1);
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), buffer.cast::<u8>(), count);
        *buffer.add(count) = 0;
    }
}

fn runtime_error_message(error: &RuntimeError) -> String {
    unsafe { CStr::from_ptr(error.message.as_ptr()) }
        .to_string_lossy()
        .into_owned()
}

fn run_gateway(config: &VexfsNfsGatewayConfig) -> Result<(), String> {
    if config.abi_version != RUNTIME_ABI_VERSION {
        return Err("NFS gateway ABI mismatch".to_string());
    }
    let backend = config_string(config.backend, "backend")?;
    let connection = config_string(config.connection, "connection")?;
    let workspace = config_string(config.workspace, "workspace")?;
    let principal = config_string(config.principal, "principal")?;
    let listen = config_string(config.listen_address, "listen address")?;
    if listen != "127.0.0.1" {
        return Err("NFS gateway may only bind 127.0.0.1".to_string());
    }
    if config.port == 0 {
        return Err("NFS gateway port is required".to_string());
    }
    let postgresql_backend = backend == "postgresql";
    let backend_c = CString::new(backend).map_err(|_| "backend contains NUL")?;
    let connection_c = CString::new(connection).map_err(|_| "connection contains NUL")?;
    let workspace_c = CString::new(workspace).map_err(|_| "workspace contains NUL")?;
    let principal_c = CString::new(principal).map_err(|_| "principal contains NUL")?;
    let runtime_config = RuntimeConfig {
        abi_version: RUNTIME_ABI_VERSION,
        backend: backend_c.as_ptr(),
        connection: connection_c.as_ptr(),
        workspace: workspace_c.as_ptr(),
        principal: principal_c.as_ptr(),
        operation_timeout_ms: config.operation_timeout_ms.max(1),
        flags: RUNTIME_EXCLUSIVE_GATEWAY,
    };
    let mut error = empty_error();
    let mut session = ptr::null_mut();
    let status = unsafe { vexfs_mount_session_open(&runtime_config, &mut session, &mut error) };
    if status != 0 {
        let message = runtime_error_message(&error);
        return Err(if message.is_empty() {
            format!("runtime session open failed with status {status}")
        } else {
            message
        });
    }
    let mut state = RuntimeState {
        session,
        postgresql_backend,
        root_inode: 1,
        uid: unsafe { libc::getuid() },
        gid: unsafe { libc::getgid() },
        strict_durability: std::env::var_os("VEXFS_NFS_STRICT_DURABILITY")
            .is_some_and(|value| value != "0"),
        next_sidecar_inode: SIDECAR_INODE_START,
        sidecars_by_key: HashMap::new(),
        sidecars: HashMap::new(),
        pending_writes: HashMap::new(),
        paths: HashMap::new(),
        inodes_by_path: HashMap::new(),
        stats: HashMap::new(),
        directory_entries: HashMap::new(),
        metadata_cache_checked_at_ms: now_ms(),
    };
    let root = state
        .stat_path("/")
        .map_err(|status| format!("root stat failed: {status:?}"))?;
    state.root_inode = root.inode as u64;
    let prefix = format!("nfs-{}-{}", unsafe { libc::getpid() }, now_ms());
    let shared_state = Arc::new(Mutex::new(state));
    let filesystem = VexfsNfs {
        state: Arc::clone(&shared_state),
        request_prefix: prefix,
        request_sequence: AtomicU64::new(1),
    };
    let bind_address = format!("{listen}:{}", config.port);
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
        .map_err(|error| error.to_string())?;
    runtime.block_on(async move {
        if postgresql_backend {
            let keepalive_state = Arc::clone(&shared_state);
            tokio::spawn(async move {
                let mut interval = tokio::time::interval(std::time::Duration::from_millis(
                    MOUNT_SESSION_KEEPALIVE_INTERVAL_MS,
                ));
                // interval's first tick is immediate; the session was just opened,
                // so wait for one full period before checking whether renewal is due.
                interval.tick().await;
                let mut failing = false;
                loop {
                    interval.tick().await;
                    let session = {
                        let Ok(state) = keepalive_state.lock() else {
                            return;
                        };
                        state.session as usize
                    };
                    let mut error = empty_error();
                    let status = unsafe {
                        vexfs_mount_session_keepalive(
                            session as *mut RuntimeSessionOpaque,
                            &mut error,
                        )
                    };
                    if status == 0 {
                        if failing {
                            eprintln!("vexfs-nfs-gateway: mount session keepalive recovered");
                        }
                        failing = false;
                    } else if !failing {
                        eprintln!(
                            "vexfs-nfs-gateway: mount session keepalive failed: {}",
                            runtime_error_message(&error)
                        );
                        failing = true;
                    }
                }
            });
        }
        let publish_state = Arc::clone(&shared_state);
        tokio::spawn(async move {
            let mut interval = tokio::time::interval(std::time::Duration::from_millis(25));
            let mut consecutive_failures = 0_u32;
            let mut next_publish_attempt_at_ms = 0_i64;
            loop {
                interval.tick().await;
                let now = now_ms();
                if now < next_publish_attempt_at_ms {
                    continue;
                }
                let publish_job = {
                    let Ok(mut state) = publish_state.lock() else {
                        return;
                    };
                    if state.postgresql_backend {
                        let candidates = state.claim_deferred_publish_batch(now);
                        if candidates.is_empty() {
                            None
                        } else {
                            Some((state.session as usize, candidates))
                        }
                    } else {
                        let cutoff = now.saturating_sub(DEFERRED_PUBLISH_IDLE_MS);
                        let idle = state
                            .pending_writes
                            .values()
                            .map(|pending| pending.updated_at_ms)
                            .max()
                            .is_some_and(|updated_at_ms| updated_at_ms <= cutoff);
                        if idle {
                            if let Err(status) =
                                state.publish_pending_batch("full", DEFERRED_PUBLISH_BATCH_SIZE)
                            {
                                eprintln!(
                                    "vexfs-nfs-gateway: deferred batch publish failed: {status:?}"
                                );
                            }
                        }
                        None
                    }
                };
                let Some((session, candidates)) = publish_job else {
                    continue;
                };
                let outcome =
                    publish_claimed_batch(session as *mut RuntimeSessionOpaque, &candidates);
                let Ok(mut state) = publish_state.lock() else {
                    return;
                };
                let mut publish_failed = false;
                if let Err(status) = state.complete_publish_claims(&candidates, &outcome.published)
                {
                    eprintln!("vexfs-nfs-gateway: deferred publish result mismatch: {status:?}");
                    publish_failed = true;
                }
                if let Some(status) = outcome.failure {
                    eprintln!("vexfs-nfs-gateway: deferred claimed publish failed: {status:?}");
                    publish_failed = true;
                }
                if publish_failed {
                    consecutive_failures = consecutive_failures.saturating_add(1);
                    let retry_delay = deferred_publish_retry_delay_ms(consecutive_failures);
                    next_publish_attempt_at_ms = now_ms().saturating_add(retry_delay);
                } else {
                    consecutive_failures = 0;
                    next_publish_attempt_at_ms = 0;
                }
            }
        });
        let listener = NFSTcpListener::bind(&bind_address, filesystem)
            .await
            .map_err(|error| error.to_string())?;
        #[cfg(unix)]
        {
            use tokio::signal::unix::{signal, SignalKind};
            let mut terminate =
                signal(SignalKind::terminate()).map_err(|error| error.to_string())?;
            let mut interrupt =
                signal(SignalKind::interrupt()).map_err(|error| error.to_string())?;
            loop {
                tokio::select! {
                    result = listener.handle_forever() => {
                        match result {
                            Ok(()) => return Err("NFS listener stopped unexpectedly".to_string()),
                            Err(error) => {
                                // nfsserve 0.11 returns from its whole accept loop for one
                                // transient accept(2) error. A mounted filesystem must stay
                                // alive, so retain the listener and retry instead of turning a
                                // short macOS network interruption into a stale mount.
                                eprintln!("vexfs-nfs-gateway: transient accept error: {error}");
                                tokio::time::sleep(std::time::Duration::from_millis(100)).await;
                            }
                        }
                    },
                    _ = terminate.recv() => return Ok(()),
                    _ = interrupt.recv() => return Ok(()),
                }
            }
        }
        #[cfg(not(unix))]
        listener
            .handle_forever()
            .await
            .map_err(|error| error.to_string())
    })?;
    // Dropping the listener drops the filesystem and closes the runtime session.
    Ok(())
}

/// Runs the NFS gateway until it receives a termination signal.
///
/// # Safety
///
/// `config` must point to a valid `VexfsNfsGatewayConfig` for the duration of
/// this call. When non-null, `error_message` must reference a writable buffer
/// containing at least `error_message_size` bytes.
#[no_mangle]
pub unsafe extern "C" fn vexfs_nfs_gateway_run(
    config: *const VexfsNfsGatewayConfig,
    error_message: *mut c_char,
    error_message_size: usize,
) -> c_int {
    if config.is_null() {
        write_error(
            error_message,
            error_message_size,
            "gateway config is required",
        );
        return 2;
    }
    let result = std::panic::catch_unwind(|| run_gateway(unsafe { &*config }));
    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            write_error(error_message, error_message_size, &message);
            1
        }
        Err(_) => {
            write_error(error_message, error_message_size, "NFS gateway panicked");
            1
        }
    }
}

#[cfg(test)]
#[export_name = "vexfs_mount_session_close"]
extern "C" fn test_vexfs_mount_session_close(_session: *mut RuntimeSessionOpaque) {}

#[cfg(test)]
#[export_name = "vexfs_mount_session_keepalive"]
extern "C" fn test_vexfs_mount_session_keepalive(
    _session: *mut RuntimeSessionOpaque,
    _error: *mut RuntimeError,
) -> RuntimeStatus {
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    fn state_with_pending(entries: &[(u64, i64, i64, u64)]) -> RuntimeState {
        let mut pending_writes = HashMap::new();
        for (inode, updated_at_ms, first_dirty_at_ms, dirty_bytes) in entries {
            pending_writes.insert(
                *inode,
                PendingWrite {
                    handle: CString::new(format!("handle-{inode}")).unwrap(),
                    generation: 1,
                    logical_size: *dirty_bytes,
                    updated_at_ms: *updated_at_ms,
                    first_dirty_at_ms: *first_dirty_at_ms,
                    dirty_bytes: *dirty_bytes,
                    publishing_generation: None,
                },
            );
        }
        RuntimeState {
            session: ptr::null_mut(),
            postgresql_backend: true,
            root_inode: 1,
            uid: 0,
            gid: 0,
            strict_durability: false,
            next_sidecar_inode: SIDECAR_INODE_START,
            sidecars_by_key: HashMap::new(),
            sidecars: HashMap::new(),
            pending_writes,
            paths: HashMap::new(),
            inodes_by_path: HashMap::new(),
            stats: HashMap::new(),
            directory_entries: HashMap::new(),
            metadata_cache_checked_at_ms: 0,
        }
    }

    #[test]
    fn foreground_write_burst_is_coalesced_until_globally_idle() {
        let now = 10_000;
        let mut state = state_with_pending(&[
            (
                1,
                now - DEFERRED_PUBLISH_IDLE_MS,
                now - DEFERRED_PUBLISH_IDLE_MS,
                4_096,
            ),
            (2, now, now, 4_096),
        ]);
        let claimed = state.claim_deferred_publish_batch(now);
        assert!(claimed.is_empty());
        assert_eq!(
            state.pending_writes.get(&1).unwrap().publishing_generation,
            None
        );
        assert_eq!(
            state.pending_writes.get(&2).unwrap().publishing_generation,
            None
        );

        let claimed = state.claim_deferred_publish_batch(now + DEFERRED_PUBLISH_IDLE_MS);
        assert_eq!(claimed.len(), 2);
    }

    #[test]
    fn deferred_publish_failures_back_off_and_cap() {
        assert_eq!(deferred_publish_retry_delay_ms(1), 250);
        assert_eq!(deferred_publish_retry_delay_ms(2), 500);
        assert_eq!(deferred_publish_retry_delay_ms(3), 1_000);
        assert_eq!(deferred_publish_retry_delay_ms(5), 4_000);
        assert_eq!(deferred_publish_retry_delay_ms(6), 5_000);
        assert_eq!(deferred_publish_retry_delay_ms(100), 5_000);
    }

    #[test]
    fn dirty_byte_threshold_flushes_before_idle_timeout() {
        let now = 20_000;
        let mut state = state_with_pending(&[(7, now, now, DEFERRED_PUBLISH_DIRTY_BYTES)]);
        let claimed = state.claim_deferred_publish_batch(now);
        assert!(claimed.is_empty());
        let claimed = state.claim_deferred_publish_batch(now + DEFERRED_PUBLISH_IDLE_MS);
        assert_eq!(claimed.len(), 1);
        assert_eq!(claimed[0].inode, 7);
    }

    #[test]
    fn oldest_dirty_write_has_a_bounded_publish_delay() {
        let now = 30_000;
        let mut state = state_with_pending(&[(
            9,
            now - DEFERRED_PUBLISH_IDLE_MS,
            now - DEFERRED_PUBLISH_MAX_AGE_MS,
            1,
        )]);
        let claimed = state.claim_deferred_publish_batch(now);
        assert_eq!(claimed.len(), 1);
        state.release_publish_claims(&claimed);
        assert_eq!(
            state.pending_writes.get(&9).unwrap().publishing_generation,
            None
        );
    }

    #[test]
    fn file_count_threshold_bounds_a_continuous_small_file_burst() {
        let now = 40_000;
        let entries: Vec<_> = (0..DEFERRED_PUBLISH_FILE_THRESHOLD)
            .map(|inode| {
                let updated = if inode + 1 == DEFERRED_PUBLISH_FILE_THRESHOLD {
                    now
                } else {
                    now - DEFERRED_PUBLISH_IDLE_MS
                };
                (inode as u64 + 1, updated, updated, 2)
            })
            .collect();
        let mut state = state_with_pending(&entries);
        let claimed = state.claim_deferred_publish_batch(now);
        assert_eq!(claimed.len(), DEFERRED_PUBLISH_BATCH_SIZE as usize);
        assert!(claimed
            .iter()
            .all(|candidate| { candidate.inode != DEFERRED_PUBLISH_FILE_THRESHOLD as u64 }));
    }

    #[test]
    fn partial_publish_clears_success_and_releases_remainder() {
        let now = 50_000;
        let mut state = state_with_pending(&[
            (1, now - DEFERRED_PUBLISH_IDLE_MS, now - 1_000, 4_096),
            (2, now - DEFERRED_PUBLISH_IDLE_MS, now - 1_000, 4_096),
        ]);
        let claimed = state.claim_deferred_publish_batch(now);
        assert_eq!(claimed.len(), 2);
        let first = &claimed[0];
        let published = [PublishedClaim {
            handle: first.handle.to_str().unwrap().to_owned(),
            generation: first.generation,
            version: 1,
        }];
        assert_eq!(
            state.complete_publish_claims(&claimed, &published).unwrap(),
            1
        );
        assert!(!state.pending_writes.contains_key(&first.inode));
        assert_eq!(
            state
                .pending_writes
                .get(&claimed[1].inode)
                .unwrap()
                .publishing_generation,
            None
        );
    }
}
