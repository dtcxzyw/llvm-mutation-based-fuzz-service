import os
import subprocess


def check_once_impl(
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
):
    try:
        filename = f"{recipe}-{id}"
        src = os.path.join(work_dir, f"{recipe}-{id}.src.ll")
        tgt = os.path.join(work_dir, f"{recipe}-{id}.tgt.ll")
        tgt2 = os.path.join(work_dir, f"{recipe}-{id}.tgt2.ll")
        subprocess.check_call([mutate_bin, seeds, src, recipe])
        try:
            subprocess.check_call(
                [llvm_opt, "-S", "-o", tgt, src, "-passes=" + pass_name],
                timeout=60,
                stderr=subprocess.DEVNULL,
            )
        except subprocess.TimeoutExpired:
            return filename, True, "timeout"
        except Exception:
            return filename, True, "crash"

        if recipe == "correctness":
            try:
                out = subprocess.check_output(
                    [alive2_tv, "--smt-to=100", "--disable-undef-input", src, tgt],
                    timeout=60,
                ).decode()
                if "0 incorrect transformations" not in out:
                    return filename, True, ""
            except subprocess.TimeoutExpired:
                pass
            except Exception:
                return filename, True, "alive2 crash"
        elif recipe == "commutative" or recipe == "canonical-form":
            funcname = compare(seeds_ref, tgt, None)
            if funcname:
                return (
                    filename,
                    True,
                    src + ":" + funcname + " is not optimized as well.",
                )
        elif recipe == "multi-use":
            funcname = compare(src, tgt, seeds_ref)
            if funcname:
                return (
                    filename,
                    True,
                    tgt + ":" + funcname + " has more instructions than before.",
                )
        elif recipe == "flag-preserving":
            subprocess.check_call([mutate_bin, tgt, tgt2, recipe])
            out = subprocess.check_output(
                [alive2_tv, "--smt-to=100", "--disable-undef-input", src, tgt2],
                timeout=60,
            ).decode()
            assert "(syntactically equal)" not in out
            if "Transformation seems to be correct" in out:
                return filename, True, ""
        else:
            return filename, False, ""
    except Exception:
        pass

    if os.path.exists(src):
        os.remove(src)
    if os.path.exists(tgt):
        os.remove(tgt)
    if os.path.exists(tgt2):
        os.remove(tgt2)
    return filename, False, ""
