import os
import sys
import subprocess
import shutil
import re
from multiprocessing import Pool
import time

alive2_tv = sys.argv[1]
llvm_bin = sys.argv[2]
llvm_opt = os.path.join(llvm_bin, 'opt')
llvm_extract = os.path.join(llvm_bin, 'llvm-extract')
patched_llvm_src = sys.argv[3]
tool_bin = sys.argv[4]
mutate_bin = os.path.join(tool_bin, 'mutate')
merge_bin = os.path.join(tool_bin, 'merge')
cost_bin = os.path.join(tool_bin, 'cost')
patch_file = sys.argv[5]
work_dir = "fuzz"
fuzz_mode = os.environ['FUZZ_MODE']

keywords = [
('test/Transforms/InstCombine', 'instcombine'),
('test/Transforms/InstSimplify', 'instcombine'),
('test/Analysis/ValueTracking', 'instcombine'),
('test/Transforms/ConstraintElimination', 'constraint-elimination'),
('test/Transforms/EarlyCSE', 'early-cse'),
('test/Transforms/GVN', 'gvn'),
('test/Transforms/NewGVN', 'newgvn'),
('test/Transforms/Reassociate', 'reassociate'),
('test/Transforms/SCCP', 'sccp'),
('test/Transforms/CorrelatedValuePropagation', 'correlated-propagation'),
('test/Transforms/SimplifyCFG', 'simplifycfg'),
('PhaseOrdering', 'default<O3>'),
]

def is_interesting():
    diff_files = subprocess.check_output(['lsdiff', patch_file]).decode()
    for keyword, pass_name in keywords:
        if keyword in diff_files:
            return pass_name
    return None

pass_name = is_interesting()
if pass_name is None:
    print('Not interesting')
    exit(0)

if pass_name == 'ValueTracking':
    pass_name = 'instcombine'
pass_name = pass_name.lower()

if os.path.exists(work_dir):
    shutil.rmtree(work_dir)
os.makedirs(work_dir)

def collect_seeds():
    seeds = set()
    current_file = ""
    pattern = re.compile(r'define .+ @(\w+)')

    with open(patch_file, 'r') as f:
        for line in f:
            if line.startswith('diff --git a/'):
                current_file = line.removeprefix('diff --git a/').split(' ')[0]
                continue
            if current_file.endswith('.ll'):
                matched = re.search(pattern, line)
                if not matched:
                    continue
                func_name = matched.group(1)
                seeds.add((current_file, func_name))
    return seeds

# Extract seeds
os.makedirs(os.path.join(work_dir, 'seeds'))
seeds = collect_seeds()
if len(seeds) == 0:
    print('No seeds found')
    exit(0)
seeds_count = len(seeds)

cnt = 0
for file, func in seeds:
    subprocess.run([llvm_extract, '-S', '-func', func, '-o', os.path.join(work_dir, 'seeds', f'seed{cnt}.ll'), os.path.join(patched_llvm_src, file)])
    cnt += 1

# Merge seeds into one file
seeds = os.path.join(work_dir, 'seeds.ll')
seeds_ref = os.path.join(work_dir, 'seeds_ref.ll')
subprocess.check_call([merge_bin, os.path.join(work_dir, 'seeds'), seeds])
subprocess.check_call([llvm_opt, '-S', '-o', seeds_ref, seeds, '-passes='+pass_name])

# Checks
recipe = ""

def parse_cost(output: str):
    res = dict()
    for line in output.splitlines():
        k,v = line.strip().split(' ')
        res[k] = int(v)
    return res

ref_cost = parse_cost(subprocess.check_output([cost_bin, seeds_ref]).decode())

def compare(before, after):
    if before == seeds_ref:
        before_cost = ref_cost
    else:
        before_cost = parse_cost(subprocess.check_output([cost_bin, before]).decode())
    after_cost = parse_cost(subprocess.check_output([cost_bin, after]).decode())

    for k in after_cost.keys():
        if k not in before_cost:
            continue
        # print(k, before_cost[k], after_cost[k])
        if before_cost[k] < after_cost[k]:
            return True 
    return False

def check_once(id):
    try:
        src = os.path.join(work_dir, f"{recipe}-{id}.src.ll")
        tgt = os.path.join(work_dir, f"{recipe}-{id}.tgt.ll")
        tgt2 = os.path.join(work_dir, f"{recipe}-{id}.tgt2.ll")
        subprocess.check_call([mutate_bin, seeds, src, recipe])
        try:
            subprocess.check_call([llvm_opt, '-S', '-o', tgt, src, '-passes='+pass_name],timeout=60)
        except Exception:
            return True
        
        if recipe == "correctness":
            try:
                out = subprocess.check_output([alive2_tv, '--smt-to=100', '--disable-undef-input', src, tgt], timeout=60).decode()
                if '0 incorrect transformations' not in out:
                    return True
            except subprocess.TimeoutExpired:
                pass
            except Exception:
                return True
        elif recipe == "commutative" or recipe == "canonical-form":
            if compare(seeds_ref, tgt):
                return True
        elif recipe == "multi-use":
            if compare(src, tgt):
                return True
        elif recipe == "flag-preserving":
            subprocess.check_call([mutate_bin, tgt, tgt2, recipe])
            out = subprocess.check_output([alive2_tv, '--smt-to=100', '--disable-undef-input', src, tgt2], timeout=60).decode()
            assert '(syntactically equal)' not in out
            if 'Transformation seems to be correct' in out:
                return True
        else:
            return False
    except Exception:
        pass
    
    if os.path.exists(src):
        os.remove(src)
    if os.path.exists(tgt):
        os.remove(tgt)
    if os.path.exists(tgt2):
        os.remove(tgt2)
    return False

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
            for res in pool.imap_unordered(check_once, range(idx, idx + files_per_iter)):
                final_res |= res
            if final_res:
                return True
            idx += files_per_iter
    return False

def print_check(name, res):
    print(" ", "\u274c" if res else "\u2705", name)

print("Seeds: {}".format(seeds_count))
print("Pass: `opt -passes={}`".format(pass_name))
print("Baseline: https://github.com/llvm/llvm-project/commit/{}".format(os.environ["LLVM_REVISION"]))
print("Patch URL: {}".format(os.environ["COMMIT_URL"]))
print("Patch SHA256: {}".format(os.environ["PATCH_SHA256"]))
start = time.time()

print("Checklist:")
scale = 0.01 if fuzz_mode == 'quickfuzz' else 1.0
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
print("Time: {}".format(time.strftime("%H:%M:%S", time.gmtime(end-start))))
