import ndt_compute

stats = ndt_compute.arrow_text_dump(
    input_path="/mnt/nvme/openwebtext_for_ndt/data-00000-of-00080.arrow",
    output_path="/mnt/nvme/openwebtext_bin/data-00000-of-00080.bin",
    column="text",
    delimiter="\n",
    max_rows=10000
)
print(f"Extracted {stats['rows']} rows.")
