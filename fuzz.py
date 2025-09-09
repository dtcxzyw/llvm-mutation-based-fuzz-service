import os
import sys
import subprocess
import shutil
import re
from multiprocessing import Pool
import time
from check import check_once_impl

alive2_tv = sys.argv[1]
llvm_bin = sys.argv[2]
llvm_opt = os.path.join(llvm_bin, "opt")
llvm_extract = os.path.join(llvm_bin, "llvm-extract")
patched_llvm_src = sys.argv[3]
tool_bin = sys.argv[4]
mutate_bin = os.path.join(tool_bin, "mutate")
merge_bin = os.path.join(tool_bin, "merge")
cost_bin = os.path.join(tool_bin, "cost")
patch_file = sys.argv[5]
work_dir = "fuzz"
fuzz_mode = os.environ["FUZZ_MODE"]

keywords = [
    ("test/Transforms/InstCombine", "instcombine<no-verify-fixpoint>"),
    ("test/Transforms/InstSimplify", "instcombine<no-verify-fixpoint>"),
    ("test/Analysis/ValueTracking", "instcombine<no-verify-fixpoint>"),
    ("test/Transforms/ConstraintElimination", "constraint-elimination"),
    ("test/Transforms/EarlyCSE", "early-cse"),
    ("test/Transforms/GVN", "gvn"),
    ("test/Transforms/NewGVN", "newgvn"),
    ("test/Transforms/Reassociate", "reassociate"),
    ("test/Transforms/SCCP", "sccp"),
    ("test/Transforms/CorrelatedValuePropagation", "correlated-propagation"),
    ("test/Transforms/SimplifyCFG", "simplifycfg"),
    ("test/Transforms/VectorCombine", "vector-combine"),
    ("PhaseOrdering", "default<O3>"),
]


def is_interesting():
    diff_files = subprocess.check_output(["lsdiff", patch_file]).decode()
    for keyword, pass_name in keywords:
        if keyword in diff_files:
            return pass_name
    return None


pass_name = is_interesting()
if pass_name is None:
    print("Not interesting")
    exit(0)

if pass_name == "ValueTracking":
    pass_name = "instcombine"
pass_name = pass_name.lower()

if os.path.exists(work_dir):
    shutil.rmtree(work_dir)
os.makedirs(work_dir)


def collect_seeds():
    seeds = set()
    current_file = ""
    pattern = re.compile(r"define .+ @([-.\w]+)\(")

    with open(patch_file, "r") as f:
        for line in f:
            if line.startswith("diff --git a/"):
                current_file = line.removeprefix("diff --git a/").split(" ")[0]
                continue
            if current_file.endswith(".ll"):
                matched = re.search(pattern, line)
                if not matched:
                    continue
                func_name = matched.group(1)
                seeds.add((current_file, func_name))
    return seeds


# Extract seeds
os.makedirs(os.path.join(work_dir, "seeds"))
seeds = collect_seeds()
if len(seeds) == 0:
    print("No seeds found")
    exit(0)
seeds_count = len(seeds)

cnt = 0
for file, func in seeds:
    subprocess.run(
        [
            llvm_extract,
            "-S",
            "-func",
            func,
            "-o",
            os.path.join(work_dir, "seeds", f"seed{cnt}.ll"),
            os.path.join(patched_llvm_src, file),
        ]
    )
    cnt += 1

# Merge seeds into one file
seeds = os.path.join(work_dir, "seeds.ll")
seeds_ref = os.path.join(work_dir, "seeds_ref.ll")
subprocess.check_call([merge_bin, os.path.join(work_dir, "seeds"), seeds])
subprocess.check_call([llvm_opt, "-S", "-o", seeds_ref, seeds, "-passes=" + pass_name])

# Checks
recipe = ""


def parse_cost(output: str):
    res = dict()
    for line in output.splitlines():
        k, v = line.strip().split(" ")
        res[k] = int(v)
    return res


ref_cost = parse_cost(subprocess.check_output([cost_bin, seeds_ref]).decode())


def compare(before, after, precond):
    if before == seeds_ref:
        before_cost = ref_cost
    else:
        before_cost = parse_cost(subprocess.check_output([cost_bin, before]).decode())
    after_cost = parse_cost(subprocess.check_output([cost_bin, after]).decode())
    if precond is not None:
        if precond == seeds_ref:
            precond_cost = ref_cost
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
            return k
    return None


def check_once(id):
    return check_once_impl(
        id,
        work_dir,
        recipe,
        seeds,
        seeds_ref,
        mutate_bin,
        llvm_opt,
        alive2_tv,
        pass_name,
        compare,
    )


def check(recipe_arg, time_budget):
    global recipe
    recipe = recipe_arg

    start = time.time()
    processes = os.cpu_count()
    files_per_iter = 20 * processes
    idx = 0
    with Pool(processes) as pool:
        while time.time() - start < time_budget:
            final_res = False
            reason_dict = dict()
            for filename, res, reason in pool.imap_unordered(
                check_once, range(idx, idx + files_per_iter)
            ):
                final_res |= res
                if res:
                    reason_dict[filename] = reason
                    break
            if final_res:
                # only keep at most 1 file
                cnt = 1
                kept_files = []
                for file in os.listdir(work_dir):
                    if file.startswith(recipe):
                        name = file.split(".")[0]
                        if name in kept_files:
                            continue
                        cnt -= 1
                        if cnt >= 0 and name in reason_dict:
                            kept_files.append(name)
                            if reason_dict[name] != "":
                                print(name, reason_dict[name])
                        else:
                            os.remove(os.path.join(work_dir, file))
                return True
            idx += files_per_iter
    return False


def print_check(name, res):
    print(" ", "\u274c" if res else "\u2705", name)


print("Seeds: {}".format(seeds_count))
print("Pass: `opt -passes={}`".format(pass_name))
print(
    "Baseline: https://github.com/llvm/llvm-project/commit/{}".format(
        os.environ["LLVM_REVISION"]
    )
)
print("Patch URL: {}".format(os.environ["COMMIT_URL"]))
print("Patch SHA256: {}".format(os.environ["PATCH_SHA256"]))
start = time.time()

print("Checklist:")
scale = 0.01 if fuzz_mode == "quickfuzz" else 1.0
# Correctness check
print_check("Correctness", check("correctness", 3600 * scale))

# Generalization check

## Commutative check
print_check("Commutative op handling", check("commutative", 300 * scale))
## Multi-use check
print_check("Multi-use handling", check("multi-use", 300 * scale))
## Flag preservation check
print_check("Flag preservation", check("flag-preserving", 300 * scale))
## Canonical form check
print_check("Canonical form handling", check("canonical-form", 300 * scale))
## TODO: Vector
## TODO: Drop constraints

end = time.time()
print("Time: {}".format(time.strftime("%H:%M:%S", time.gmtime(end - start))))
