import os
import sys
import subprocess
import shutil
from multiprocessing import Pool
from check import check_once_impl
import random
import tqdm

alive2_tv = sys.argv[1]
llvm_bin = sys.argv[2]
llvm_opt = os.path.join(llvm_bin, "opt")
tool_bin = sys.argv[3]
mutate_bin = os.path.join(tool_bin, "mutate")
merge_bin = os.path.join(tool_bin, "merge")
cost_bin = os.path.join(tool_bin, "cost")
work_dir = "fuzz"
pass_name = "instcombine<no-verify-fixpoint>"
test_dir = sys.argv[4]
test_count = int(sys.argv[5])
processes = 16

if os.path.exists(work_dir):
    shutil.rmtree(work_dir)
os.makedirs(work_dir)
seed_dir = os.path.join(work_dir, "seed")
os.makedirs(seed_dir)

block_list = [
    "minmax-fold.ll",  # Known FP issue
    "clamp-to-minmax.ll",
    "loadstore-metadata.ll",  # noalias.addrspace
    "fabs.ll",
    "fpcast.ll",
    "fcmp.ll",
    "sign-test-and-or.ll",  # https://alive2.llvm.org/ce/z/vbzktq
    "2008-01-13-AndCmpCmp.ll",  # https://alive2.llvm.org/ce/z/-eysFm
    "icmp-equality-test.ll",  # https://github.com/llvm/llvm-project/issues/121702
    "preserve-sminmax.ll",  # https://github.com/llvm/llvm-project/issues/121772
    "select-imm-canon.ll",  # https://github.com/llvm/llvm-project/issues/121774
    "select_meta.ll",
    "sadd_sat.ll",
    "unsigned_saturated_sub.ll",
    "matching-binops.ll",
    "minmax-fp.ll",  # https://github.com/llvm/llvm-project/issues/121786
    "fast-basictest.ll",  # https://github.com/llvm/llvm-project/issues/121790
    "fneg-fabs.ll",  # https://github.com/AliveToolkit/alive2/pull/1155
    "simplify-demanded-fpclass.ll",  # https://github.com/AliveToolkit/alive2/pull/1155
    "unordered-fcmp-select.ll",  # https://alive2.llvm.org/ce/z/xz-He7
    "fcmp-select.ll",  # https://github.com/llvm/llvm-project/issues/136430
    "intrinsic-select.ll",  # https://alive2.llvm.org/ce/z/QRqNcr
    "select-binop-foldable-floating-point.ll",  # https://alive2.llvm.org/ce/z/iLmXch
    "cast.ll",  # https://alive2.llvm.org/ce/z/CD7axf
    "known-bits.ll",  # https://alive2.llvm.org/ce/z/SwGU8G
    "fpclass-from-dom-cond.ll",  # https://alive2.llvm.org/ce/z/MBP2JY
    "select-select.ll",  # https://alive2.llvm.org/ce/z/weke43
    "create-class-from-logic-fcmp.ll",  # https://alive2.llvm.org/ce/z/nQYKsP
]


def preprocess(pack):
    id, file = pack
    if file in block_list:
        return None
    if file.endswith(".ll"):
        try:
            orig = os.path.join(test_dir, file)
            tmp = os.path.join(seed_dir, f"{id}")
            os.makedirs(tmp)
            shutil.copyfile(orig, os.path.join(tmp, "orig.ll"))
            seed = os.path.join(tmp, "seed.ll")
            merge_ops = []
            # merge_ops = ['-ignore-fp']
            subprocess.check_call(
                [merge_bin, tmp, seed] + merge_ops, stderr=subprocess.DEVNULL
            )
            ref_out = os.path.join(tmp, "ref.ll")
            subprocess.check_call(
                [llvm_opt, "-passes=" + pass_name, seed, "-o", ref_out, "-S"],
                stderr=subprocess.DEVNULL,
            )
            return (seed, ref_out)
        except Exception:
            pass

    return None


tests = []
with Pool(processes) as pool:
    for res in pool.imap_unordered(preprocess, enumerate(os.listdir(test_dir))):
        if res is not None:
            tests.append(res)
print(f"Valid tests: {len(tests)}")


def parse_cost(output: str):
    res = dict()
    for line in output.splitlines():
        k, v = line.strip().split(" ")
        res[k] = int(v)
    return res


ref_cost = dict()
for k, v in tests:
    ref_cost[v] = parse_cost(subprocess.check_output([cost_bin, v]).decode())


def compare(before, after, precond):
    if before in ref_cost:
        before_cost = ref_cost[before]
    else:
        before_cost = parse_cost(subprocess.check_output([cost_bin, before]).decode())
    after_cost = parse_cost(subprocess.check_output([cost_bin, after]).decode())
    if precond is not None:
        if precond in ref_cost:
            precond_cost = ref_cost[precond]
        else:
            precond_cost = parse_cost(
                subprocess.check_output([cost_bin, precond]).decode()
            )

    for k in after_cost.keys():
        if k not in before_cost:
            continue
        # print(k, before_cost[k], after_cost[k])
        if before_cost[k] < after_cost[k]:
            if precond is not None:
                if before_cost[k] < precond_cost[k]:
                    continue
            print(k)
            return True
    return False


recipes = ["correctness", "commutative", "multi-use", "canonical-form"]


def check(id):
    # recipe = random.choice(recipes)
    recipe = recipes[0]
    seed, seed_ref = random.choice(tests)
    if check_once_impl(
        id,
        work_dir,
        recipe,
        seed,
        seed_ref,
        mutate_bin,
        llvm_opt,
        alive2_tv,
        pass_name,
        compare,
    ):
        return (id, recipe, seed)
    return None


progress = tqdm.tqdm(range(test_count))
with Pool(processes) as pool:
    for res in pool.imap_unordered(check, range(test_count)):
        progress.update()
        if res is None:
            continue
        id, recipe, seed = res
        progress.write(f"{id} {recipe} {seed}")
        # exit(1)
progress.close()
