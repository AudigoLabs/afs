use std::ops::Range;

use afs_difftest::configs::{MIN_IO, PRIMARY, SINGLE_SUB_BLOCK, SMALL, TINY};
use afs_difftest::diff::{Params, Stats, run_seed};

fn run(params: &Params, seeds: Range<u64>) -> Stats {
    let mut total = Stats::default();
    let trace = std::env::var_os("AFS_TRACE_SEEDS").is_some();
    for seed in seeds.clone() {
        if trace {
            eprintln!("{} seed {seed}", params.name);
        }
        total.merge(&run_seed(params, seed));
    }
    println!("STATS {} seeds {:?}: {total:?}", params.name, seeds);
    assert_eq!(total.seeds, seeds.end - seeds.start);
    assert_eq!(total.trace_checks, total.api_calls);
    total
}

macro_rules! seed_tests {
    ($params:expr, $($name:ident: $range:expr),* $(,)?) => {
        $(
            #[test]
            fn $name() {
                run(&$params, $range);
            }
        )*
    };
}

seed_tests!(PRIMARY,
    primary_0: 0..125, primary_1: 125..250, primary_2: 250..375, primary_3: 375..500,
    primary_4: 500..625, primary_5: 625..750, primary_6: 750..875, primary_7: 875..1000,
);

seed_tests!(SMALL,
    small_0: 0..250, small_1: 250..500, small_2: 500..750, small_3: 750..1000,
);

seed_tests!(TINY, tiny_0: 0..250, tiny_1: 250..500);

seed_tests!(MIN_IO, min_io_0: 0..250, min_io_1: 250..500);

seed_tests!(SINGLE_SUB_BLOCK, single_sub_block_0: 0..250, single_sub_block_1: 250..500);

#[test]
fn single_seed() {
    let (Some(seed), Ok(name)) = (std::env::var_os("AFS_SEED"), std::env::var("AFS_CONFIG")) else {
        return;
    };
    let seed: u64 = seed.to_str().unwrap().parse().unwrap();
    let params = [PRIMARY, SMALL, TINY, MIN_IO, SINGLE_SUB_BLOCK].into_iter().find(|p| p.name == name).unwrap();
    run(&params, seed..seed + 1);
}
