use std::ops::Range;

use afs_difftest::configs::{MIN_IO, PRIMARY, SINGLE_SUB_BLOCK, SMALL, TINY};
use afs_difftest::diff::{Params, Stats};
use afs_difftest::power_loss::run_seed;

fn run(params: &Params, seeds: Range<u64>) {
    let mut total = Stats::default();
    for seed in seeds.clone() {
        if std::env::var_os("AFS_TRACE_SEEDS").is_some() {
            eprintln!("{} seed {seed}", params.name);
        }
        total.merge(&run_seed(params, seed));
    }
    println!("STATS power_loss {} seeds {:?}: {total:?}", params.name, seeds);
}

#[test]
fn power_loss_primary() {
    run(&PRIMARY, 0..200);
}

#[test]
fn power_loss_small() {
    run(&SMALL, 0..500);
}

#[test]
fn power_loss_tiny() {
    run(&TINY, 0..500);
}

#[test]
fn power_loss_min_io() {
    run(&MIN_IO, 0..500);
}

#[test]
fn power_loss_single_sub_block() {
    run(&SINGLE_SUB_BLOCK, 0..300);
}
