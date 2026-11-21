#![doc = include_str!("../README.md")]

mod joint;
mod ffi;
pub mod multi_axis;

pub use joint::{ControlMode, Joint};
pub use yfoc_can as can;
pub use yfoc_protocol as protocol;
