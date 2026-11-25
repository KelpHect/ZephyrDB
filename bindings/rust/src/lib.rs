use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::slice;

#[repr(C)]
pub struct ZephyrDB {
    _private: [u8; 0],
}

#[repr(i32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum ZephyrStatus {
    Ok = 0,
    ErrInvalidArg = 1,
    ErrSyntax = 2,
    ErrNotFound = 3,
    ErrConstraint = 4,
    ErrIo = 5,
    ErrInternal = 6,
    ErrBusy = 7,
}

#[repr(i32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum ZephyrType {
    Null = 0,
    I64 = 1,
    F64 = 2,
    Bool = 3,
    Text = 4,
    Bytea = 5,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct ZephyrSlice {
    pub ptr: *const u8,
    pub len: u32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub union ZephyrValueUnion {
    pub i64_val: i64,
    pub f64_val: f64,
    pub boolean: u8,
    pub slice: ZephyrSlice,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct ZephyrValue {
    pub r#type: i32,
    pub _pad: u32,
    pub as_: ZephyrValueUnion,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct ZephyrColumnDesc {
    pub name: *const c_char,
    pub name_len: u32,
    pub r#type: i32,
}

#[repr(C)]
pub struct ZephyrResultSet {
    pub row_count: u64,
    pub col_count: u64,
    pub rows_affected: u64,
    pub columns: *const ZephyrColumnDesc,
    pub values: *const ZephyrValue,
    pub _arena_handle: *mut c_void,
    pub status: u32,
    pub flags: u32,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct ZephyrAggregate {
    pub count: i64,
    pub sum_i64: i64,
    pub sum_f64: f64,
    pub status: i32,
    pub _pad: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct ZephyrConfig {
    pub data_dir: *const c_char,
    pub arena_slab_bytes: u64,
    pub wal_flush_ms: u32,
    pub enable_wal: u32,
    pub enable_compactor: u32,
    pub _pad: u32,
}

#[cfg(target_pointer_width = "64")]
const _: () = {
    assert!(std::mem::size_of::<ZephyrSlice>() == 16);
    assert!(std::mem::size_of::<ZephyrValue>() == 24);
    assert!(std::mem::size_of::<ZephyrColumnDesc>() == 16);
    assert!(std::mem::size_of::<ZephyrResultSet>() == 56);
    assert!(std::mem::size_of::<ZephyrAggregate>() == 32);
    assert!(std::mem::size_of::<ZephyrConfig>() == 32);
};

extern "C" {
    pub fn zephyr_default_config() -> ZephyrConfig;
    pub fn zephyr_db_open(config: *const ZephyrConfig) -> *mut ZephyrDB;
    pub fn zephyr_db_close(db: *mut ZephyrDB);
    pub fn zephyr_exec(db: *mut ZephyrDB, sql: *const c_char, rows_affected: *mut u64) -> i32;
    pub fn zephyr_query(db: *mut ZephyrDB, sql: *const c_char) -> *mut ZephyrResultSet;
    pub fn zephyr_query_params(
        db: *mut ZephyrDB,
        sql: *const c_char,
        params: *const ZephyrValue,
        param_count: u32,
    ) -> *mut ZephyrResultSet;
    pub fn zephyr_result_free(result: *mut ZephyrResultSet);
    pub fn zephyr_aggregate_count(db: *mut ZephyrDB, table_name: *const c_char) -> ZephyrAggregate;
    pub fn zephyr_aggregate_sum(
        db: *mut ZephyrDB,
        table_name: *const c_char,
        col_name: *const c_char,
    ) -> ZephyrAggregate;
    pub fn zephyr_checkpoint(db: *mut ZephyrDB) -> i32;
    pub fn zephyr_last_error(db: *mut ZephyrDB) -> *const c_char;
    pub fn zephyr_raft_start(
        db: *mut ZephyrDB,
        node_id: u32,
        port: u16,
        peers_csv: *const c_char,
    ) -> i32;
    pub fn zephyr_raft_stop(db: *mut ZephyrDB);
    pub fn zephyr_pgwire_start(db: *mut ZephyrDB, port: u16) -> i32;
    pub fn zephyr_pgwire_stop(db: *mut ZephyrDB);
}

#[derive(Debug)]
pub struct Error {
    pub code: i32,
    pub message: String,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "ZephyrDB error ({}): {}", self.code, self.message)
    }
}

impl std::error::Error for Error {}

pub struct Database {
    ptr: *mut ZephyrDB,
}

unsafe impl Send for Database {}
unsafe impl Sync for Database {}

impl Database {
    pub fn open(data_dir: &str) -> Result<Self, Error> {
        let c_dir = CString::new(data_dir).map_err(|_| Error {
            code: ZephyrStatus::ErrInvalidArg as i32,
            message: "data_dir contains NUL byte".into(),
        })?;
        unsafe {
            let mut cfg = zephyr_default_config();
            cfg.data_dir = c_dir.as_ptr();
            let ptr = zephyr_db_open(&cfg);
            if ptr.is_null() {
                return Err(Error {
                    code: ZephyrStatus::ErrIo as i32,
                    message: format!("failed to open ZephyrDB at {data_dir}"),
                });
            }
            Ok(Self { ptr })
        }
    }

    pub fn exec(&self, sql: &str) -> Result<u64, Error> {
        let c_sql = CString::new(sql).map_err(|_| Error {
            code: ZephyrStatus::ErrInvalidArg as i32,
            message: "sql contains NUL byte".into(),
        })?;
        let mut affected: u64 = 0;
        let code = unsafe { zephyr_exec(self.ptr, c_sql.as_ptr(), &mut affected) };
        if code != ZephyrStatus::Ok as i32 {
            return Err(Error {
                code,
                message: self.last_error(),
            });
        }
        Ok(affected)
    }

    pub fn query(&self, sql: &str) -> Result<ResultSet, Error> {
        let c_sql = CString::new(sql).map_err(|_| Error {
            code: ZephyrStatus::ErrInvalidArg as i32,
            message: "sql contains NUL byte".into(),
        })?;
        let rs = unsafe { zephyr_query(self.ptr, c_sql.as_ptr()) };
        if rs.is_null() {
            return Err(Error {
                code: ZephyrStatus::ErrInternal as i32,
                message: self.last_error(),
            });
        }
        let status = unsafe { (*rs).status };
        if status != ZephyrStatus::Ok as u32 {
            unsafe { zephyr_result_free(rs) };
            return Err(Error {
                code: status as i32,
                message: self.last_error(),
            });
        }
        Ok(ResultSet { ptr: rs })
    }

    pub fn aggregate_count(&self, table: &str) -> Result<ZephyrAggregate, Error> {
        let c_table = CString::new(table).map_err(|_| Error {
            code: ZephyrStatus::ErrInvalidArg as i32,
            message: "table contains NUL byte".into(),
        })?;
        Ok(unsafe { zephyr_aggregate_count(self.ptr, c_table.as_ptr()) })
    }

    pub fn last_error(&self) -> String {
        unsafe {
            let p = zephyr_last_error(self.ptr);
            if p.is_null() {
                String::new()
            } else {
                CStr::from_ptr(p).to_string_lossy().into_owned()
            }
        }
    }
}

impl Drop for Database {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            unsafe { zephyr_db_close(self.ptr) };
            self.ptr = std::ptr::null_mut();
        }
    }
}

pub struct ResultSet {
    ptr: *mut ZephyrResultSet,
}

impl ResultSet {
    pub fn row_count(&self) -> usize {
        unsafe { (*self.ptr).row_count as usize }
    }

    pub fn col_count(&self) -> usize {
        unsafe { (*self.ptr).col_count as usize }
    }

    pub fn rows_affected(&self) -> u64 {
        unsafe { (*self.ptr).rows_affected }
    }

    pub fn row(&self, idx: usize) -> RowRef<'_> {
        assert!(idx < self.row_count());
        let cols = self.col_count();
        let cells = unsafe {
            let base = (*self.ptr).values.add(idx * cols);
            slice::from_raw_parts(base, cols)
        };
        RowRef { cells }
    }
}

impl Drop for ResultSet {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            unsafe { zephyr_result_free(self.ptr) };
            self.ptr = std::ptr::null_mut();
        }
    }
}

pub struct RowRef<'a> {
    cells: &'a [ZephyrValue],
}

impl<'a> RowRef<'a> {
    pub fn get_i64(&self, col: usize) -> Option<i64> {
        let v = &self.cells[col];
        if v.r#type == ZephyrType::I64 as i32 {
            Some(unsafe { v.as_.i64_val })
        } else {
            None
        }
    }

    pub fn get_f64(&self, col: usize) -> Option<f64> {
        let v = &self.cells[col];
        if v.r#type == ZephyrType::F64 as i32 {
            Some(unsafe { v.as_.f64_val })
        } else {
            None
        }
    }

    pub fn get_bool(&self, col: usize) -> Option<bool> {
        let v = &self.cells[col];
        if v.r#type == ZephyrType::Bool as i32 {
            Some(unsafe { v.as_.boolean != 0 })
        } else {
            None
        }
    }

    pub fn get_bytes(&self, col: usize) -> Option<&'a [u8]> {
        let v = &self.cells[col];
        if v.r#type == ZephyrType::Text as i32 || v.r#type == ZephyrType::Bytea as i32 {
            let s = unsafe { v.as_.slice };
            if s.ptr.is_null() || s.len == 0 {
                Some(&[])
            } else {
                Some(unsafe { slice::from_raw_parts(s.ptr, s.len as usize) })
            }
        } else {
            None
        }
    }

    pub fn get_str(&self, col: usize) -> Option<&'a str> {
        self.get_bytes(col)
            .and_then(|b| std::str::from_utf8(b).ok())
    }
}
