//! 上位机内置烧录。通过 probe-rs 用 SWD 把 ELF、HEX 或 BIN 写进板载 Flash。

mod board;
mod image;
mod run;

pub use board::Board;
pub use image::check_image;
pub use run::{flash_image, list_probes, FlashJob, ProbeId, Progress};
