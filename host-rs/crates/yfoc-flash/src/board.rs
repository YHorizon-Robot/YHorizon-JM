//! YH_JM 的 Flash 布局。容量和标定页与链接脚本一致。

use probe_rs::config::{MemoryRegion, Registry};

/** @brief Flash 起始地址。 */
pub const FLASH_BASE: u64 = 0x0800_0000;

/** @brief 标定页长度。最后一页是 2 KB。 */
pub const NV_LEN: u64 = 0x800;

/**
 * @brief 可烧录的板卡。
 *
 * 本仓库只发布 YH_JM（GD32F303CCT6）。
 */
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Board {
    YhJm,
}

impl Board {
    pub const ALL: [Board; 1] = [Board::YhJm];

    /** @brief 界面上的板卡名。 */
    pub fn label(self) -> &'static str {
        match self {
            Board::YhJm => "YH_JM",
        }
    }

    /** @brief 芯片简称，用于界面说明。 */
    pub fn chip_label(self) -> &'static str {
        match self {
            Board::YhJm => "GD32F303CCT6",
        }
    }

    /** @brief 整片 Flash 字节数。 */
    pub fn flash_size(self) -> u64 {
        match self {
            Board::YhJm => 256 * 1024,
        }
    }

    /** @brief 标定页地址。链接脚本把代码区停在这一页之前。 */
    pub fn nv_addr(self) -> u64 {
        FLASH_BASE + self.flash_size() - NV_LEN
    }

    /// 起始地址包含两页参数记录和最后一页旧版标定。
    pub fn parameter_addr(self) -> u64 {
        self.nv_addr() - 2 * NV_LEN
    }

    /** @brief SWD 时钟，kHz。沿用现有烧录脚本的 500 kHz。 */
    pub fn speed_khz(self) -> u32 {
        match self {
            Board::YhJm => 500,
        }
    }
}

/**
 * @brief 解析 probe-rs 里的芯片名，并核对 Flash 为 256 KB。
 */
pub fn attach_name(board: Board) -> Result<String, String> {
    let registry = Registry::from_builtin_families();
    match board {
        Board::YhJm => {
            let name = "GD32F303CC";
            let target = registry.get_target_by_name(name).map_err(|err| {
                format!("目标库里没有 {name}: {err}")
            })?;
            ensure_size(board, name, boot_flash_bytes(&target))?;
            Ok(name.to_owned())
        }
    }
}

fn ensure_size(board: Board, name: &str, bytes: u64) -> Result<(), String> {
    if bytes == board.flash_size() {
        Ok(())
    } else {
        Err(format!(
            "{name} 的 Flash 是 {bytes} 字节，与 {} 的 {} 字节不一致",
            board.label(),
            board.flash_size()
        ))
    }
}

/** @brief 从 0x08000000 启动的那片 Flash，不含选项字节。 */
fn boot_flash_bytes(target: &probe_rs::Target) -> u64 {
    target
        .memory_map
        .iter()
        .find_map(|region| match region {
            MemoryRegion::Nvm(nvm) if nvm.range.start == FLASH_BASE => {
                Some(nvm.range.end.saturating_sub(nvm.range.start))
            }
            _ => None,
        })
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn registry_matches_board_flash() {
        for board in Board::ALL {
            let name = attach_name(board).unwrap_or_else(|err| panic!("{err}"));
            assert!(!name.is_empty(), "{}", board.label());
        }
    }

    #[test]
    fn nv_page_is_last_sector() {
        assert_eq!(Board::YhJm.nv_addr(), 0x0803_F800);
        assert_eq!(Board::YhJm.parameter_addr(), 0x0803_E800);
    }
}
