//! 在连接探针前解析并检查固件；校验与编程使用相同的地址和数据。
use std::{fs, path::Path};

use object::read::elf::{ElfFile32, ProgramHeader};
use object::{Architecture, LittleEndian, Object};

use crate::board::{Board, FLASH_BASE};

pub(crate) struct ImageBlock {
    pub address: u64,
    pub data: Vec<u8>,
}

pub fn check_image(path: &Path, board: Board, overwrite_nv: bool) -> Result<(), String> {
    read_image(path, board, overwrite_nv).map(|_| ())
}

pub(crate) fn read_image(path: &Path, board: Board, overwrite_nv: bool) -> Result<Vec<ImageBlock>, String> {
    let ext = path.extension().and_then(|s| s.to_str()).unwrap_or("").to_ascii_lowercase();
    let io_error = |err| format!("读取固件 {} 失败: {err}", path.display());
    let blocks = match ext.as_str() {
        "bin" => {
            let size = fs::metadata(path).map_err(io_error)?.len();
            check_range(FLASH_BASE, size, board, overwrite_nv)?;
            vec![ImageBlock { address: FLASH_BASE, data: fs::read(path).map_err(io_error)? }]
        }
        "hex" => parse_hex(&fs::read_to_string(path).map_err(io_error)?)?,
        "elf" => parse_elf(&fs::read(path).map_err(io_error)?)?,
        _ => return Err("固件格式须为 ELF、HEX 或 BIN".into()),
    };
    if blocks.is_empty() || blocks.iter().all(|block| block.data.is_empty()) {
        return Err("固件没有可写入的数据".into());
    }
    for block in &blocks {
        check_range(block.address, block.data.len() as u64, board, overwrite_nv)?;
    }
    Ok(blocks)
}

fn check_range(start: u64, len: u64, board: Board, overwrite_nv: bool) -> Result<(), String> {
    let end = start.checked_add(len).ok_or("固件地址溢出")?;
    if start < FLASH_BASE || end > FLASH_BASE + board.flash_size() {
        return Err(format!("镜像 0x{start:08X}..0x{end:08X} 超出 {} 主 Flash", board.label()));
    }
    if !overwrite_nv && end > board.parameter_addr() {
        return Err(format!("镜像盖住电机参数或标定页 0x{:08X}。如需写入，请勾选覆盖参数区", board.parameter_addr()));
    }
    Ok(())
}

fn parse_elf(bytes: &[u8]) -> Result<Vec<ImageBlock>, String> {
    let file = ElfFile32::<LittleEndian>::parse(bytes).map_err(|e| format!("ELF 无法解析: {e}"))?;
    if file.architecture() != Architecture::Arm {
        return Err("请选择 ARM Cortex-M 固件".into());
    }
    let endian = file.endian();
    let mut blocks = Vec::new();
    for segment in file.elf_program_headers() {
        if segment.p_type(endian) != object::elf::PT_LOAD || segment.p_filesz(endian) == 0 {
            continue;
        }
        // .data 在 RAM 中运行，初始化字节存放在 p_paddr 指定的 Flash 地址。
        let data = segment.data(endian, bytes).map_err(|_| "ELF 加载段超出文件")?;
        blocks.push(ImageBlock { address: segment.p_paddr(endian).into(), data: data.to_vec() });
    }
    Ok(blocks)
}

fn parse_hex(text: &str) -> Result<Vec<ImageBlock>, String> {
    let mut base = 0u64;
    let mut blocks = Vec::new();
    for (i, record) in ihex::Reader::new(text).enumerate() {
        match record.map_err(|err| format!("HEX 第 {} 行: {err}", i + 1))? {
            ihex::Record::ExtendedLinearAddress(upper) => base = u64::from(upper) << 16,
            ihex::Record::ExtendedSegmentAddress(upper) => base = u64::from(upper) << 4,
            ihex::Record::Data { offset, value } if !value.is_empty() => {
                blocks.push(ImageBlock { address: base + u64::from(offset), data: value });
            }
            _ => {}
        }
    }
    Ok(blocks)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex_of(records: &[ihex::Record]) -> String {
        ihex::create_object_file_representation(records).expect("hex")
    }

    #[test]
    fn bin_inside_code_region_is_ok() {
        let dir = std::env::temp_dir();
        let path = dir.join("yfoc-flash-small.bin");
        fs::write(&path, vec![0u8; 1024]).unwrap();
        assert!(check_image(&path, Board::YhJm, false).is_ok());
        let _ = fs::remove_file(path);
    }

    #[test]
    fn bin_covering_nv_is_rejected() {
        let path = std::env::temp_dir().join("yfoc-flash-full.bin");
        fs::write(&path, vec![0u8; Board::YhJm.flash_size() as usize]).unwrap();
        let err = check_image(&path, Board::YhJm, false).unwrap_err();
        assert!(err.contains("标定页"), "{err}");
        assert!(check_image(&path, Board::YhJm, true).is_ok());
        let _ = fs::remove_file(path);
    }

    #[test]
    fn hex_nv_page_is_detected() {
        let text = hex_of(&[
            ihex::Record::ExtendedLinearAddress(0x0803),
            ihex::Record::Data { offset: 0xF800, value: vec![0x11, 0x22] },
            ihex::Record::EndOfFile,
        ]);
        let path = std::env::temp_dir().join("yfoc-flash-nv.hex");
        fs::write(&path, text).unwrap();
        let err = check_image(&path, Board::YhJm, false).unwrap_err();
        assert!(err.contains("0803F800") || err.contains("0x0803F800") || err.contains("标定页"), "{err}");
        let _ = fs::remove_file(path);
    }

    #[test]
    fn hex_in_code_region_is_ok() {
        let text = hex_of(&[
            ihex::Record::ExtendedLinearAddress(0x0800),
            ihex::Record::Data { offset: 0, value: vec![0xAA; 16] },
            ihex::Record::EndOfFile,
        ]);
        let path = std::env::temp_dir().join("yfoc-flash-code.hex");
        fs::write(&path, text).unwrap();
        assert!(check_image(&path, Board::YhJm, false).is_ok());
        let _ = fs::remove_file(path);
    }
}
