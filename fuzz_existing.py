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
llvm_opt = os.path.join(llvm_bin, 'opt')
tool_bin = sys.argv[3]
mutate_bin = os.path.join(tool_bin, 'mutate')
merge_bin = os.path.join(tool_bin, 'merge')
cost_bin = os.path.join(tool_bin, 'cost')
work_dir = "fuzz"
pass_name = 'instcombine<no-verify-fixpoint>'
test_dir = sys.argv[4]
test_count = int(sys.argv[5])
processes = 16

if os.path.exists(work_dir):
    shutil.rmtree(work_dir)
os.makedirs(work_dir)
seed_dir = os.path.join(work_dir, 'seed')
os.makedirs(seed_dir)

block_list = [
'select-cmp-cttz-ctlz.ll' # https://github.com/llvm/llvm-project/issues/121428
'minmax-fold.ll', # Known FP issue
'fneg-fabs.ll', # https://github.com/llvm/llvm-project/issues/121430
]

def preprocess(pack):
    id, file = pack
    if file in block_list:
        return None
    if file.endswith('.ll'):
        try:
            orig = os.path.join(test_dir, file)
            tmp = os.path.join(seed_dir, f'{id}')
            os.makedirs(tmp)
            shutil.copyfile(orig, os.path.join(tmp, 'orig.ll'))
            seed = os.path.join(tmp, 'seed.ll')
            subprocess.check_call([merge_bin, tmp, seed], stderr=subprocess.DEVNULL)
            ref_out = os.path.join(tmp, 'ref.ll')
            subprocess.check_call([llvm_opt,'-passes='+pass_name, seed, '-o', ref_out, '-S'], stderr=subprocess.DEVNULL)
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
        k,v = line.strip().split(' ')
        res[k] = int(v)
    return res

ref_cost = dict()
for k, v in tests:
    ref_cost[v] = parse_cost(subprocess.check_output([cost_bin, v]).decode())

def compare(before, after):
    if before in ref_cost:
        before_cost = ref_cost[before]
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

def check(id):
    recipe = 'correctness'
    seed, seed_ref = random.choice(tests)
    if check_once_impl(id, work_dir, recipe, seed, seed_ref, mutate_bin, llvm_opt, alive2_tv, pass_name, compare):
        return (id, recipe, seed)
    return None

progress = tqdm.tqdm(range(test_count))
with Pool(processes) as pool:
    for res in pool.imap_unordered(check, range(test_count)):
        progress.update()
        if res is None:
            continue
        id, recipe, seed = res
        print(id, recipe, seed)
        exit(1)
progress.close()
