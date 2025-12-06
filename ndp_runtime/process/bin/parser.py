import re
import csv
import pandas as pd

def parse_bpe_log_to_csv(input_path, output_path):
    with open(input_path, 'r') as f:
        lines = f.readlines()

    records = []
    current = {}
    stage = None

    for line in lines:
        line = line.strip()

        if "[RUSAGE] Before BPE" in line:
            current = {}
            stage = "before"

        elif "[RUSAGE] After BPE" in line:
            stage = "after"

        elif "User time:" in line:
            current[f"{stage}_user_time"] = float(line.split(":")[1].strip().split()[0])

        elif "System time:" in line:
            current[f"{stage}_sys_time"] = float(line.split(":")[1].strip().split()[0])

        elif "Max RSS:" in line:
            current[f"{stage}_rss"] = int(line.split(":")[1].strip().split()[0])

        elif "Page Faults (soft):" in line:
            current[f"{stage}_pagefault_soft"] = int(line.split(":")[1].strip())

        elif "Page Faults (hard):" in line:
            current[f"{stage}_pagefault_hard"] = int(line.split(":")[1].strip())

        elif "Voluntary Context Switches:" in line:
            current[f"{stage}_ctx_voluntary"] = int(line.split(":")[1].strip())

        elif "Involuntary Context Switches:" in line:
            current[f"{stage}_ctx_involuntary"] = int(line.split(":")[1].strip())

        elif "[PERF]" in line:
            parts = line.replace("[PERF]", "").split(":")
            if len(parts) == 2:
                key = parts[0].strip().lower().replace(" ", "_")
                current[key] = int(parts[1].strip())

        elif "토큰화 처리 시간:" in line:
            current["bpe_time_us"] = int(re.search(r"(\d+)", line).group(1))

        elif "byte_size" in line:
            current["output_size_bytes"] = int(re.search(r"byte_size: (\d+)", line).group(1))

        elif "[경과시간] spdk 응답 전송 완료" in line:
            # 계산 결과 정리
            record = {
                "cpu_time_diff_ms": current.get("after_user_time", 0) - current.get("before_user_time", 0),
                "sys_time_diff_ms": current.get("after_sys_time", 0) - current.get("before_sys_time", 0),
                "rss_diff_kb": current.get("after_rss", 0) - current.get("before_rss", 0),
                "pagefault_soft_diff": current.get("after_pagefault_soft", 0) - current.get("before_pagefault_soft", 0),
                "pagefault_hard_diff": current.get("after_pagefault_hard", 0) - current.get("before_pagefault_hard", 0),
                "ctx_voluntary_diff": current.get("after_ctx_voluntary", 0) - current.get("before_ctx_voluntary", 0),
                "ctx_involuntary_diff": current.get("after_ctx_involuntary", 0) - current.get("before_ctx_involuntary", 0),
                "bpe_time_us": current.get("bpe_time_us"),
                "output_size_bytes": current.get("output_size_bytes"),
                "instructions": current.get("instructions"),
                "cpu_cycles": current.get("cpu_cycles"),
                "branch_instructions": current.get("branch_instructions"),
                "branch_misses": current.get("branch_misses"),
                "l1d_access": current.get("l1d_access"),
                "l1d_miss": current.get("l1d_miss"),
                "l3_access": current.get("l3_access"),
                "l3_miss": current.get("l3_miss")
            }
            records.append(record)

    # 데이터프레임 생성
    df = pd.DataFrame(records)
    df.index = [f"run_{i+1}" for i in range(len(df))]
    df.to_csv(output_path)
    print(f"Saved CSV to {output_path}")

# 사용 예시
if __name__ == "__main__":
    parse_bpe_log_to_csv("result50.txt", "bpe_results_final.csv")
