//! YHorizon-JM 上位机。无命令行动作时打开图形界面。

mod cli;
mod commission;
mod scope;
mod session;
mod theme;
mod ui;
mod worker;

use clap::Parser;

fn main() {
    let args = cli::Args::parse();
    if args.gui || !args.cli_requested() {
        if let Err(err) = ui::run() {
            eprintln!("{err}");
            std::process::exit(1);
        }
    } else {
        std::process::exit(cli::run(args));
    }
}
