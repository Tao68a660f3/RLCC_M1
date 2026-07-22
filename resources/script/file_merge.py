import os

files = [
    "ASC0805.BIN",
    "ASC16.BIN",
    "ASC16S.BIN",
    "ASC1608.BIN",
    "ASC1610.BIN",
    "ASC1616.BIN",
    "ASC2010.BIN",
    "ASC2410.BIN",
    "ASC2412.BIN",
    "GBK1616H.BIN",
    "GBK1616S.BIN",
    "GBK1624M.BIN",
    "GBK1624Y.BIN",
    "GBK2432S.BIN",
    "GBK2432H.BIN",
]

base_path = r"..\fonts\\"
output_file = os.path.join(base_path, "W25Q64_FONT_IMAGE.BIN")
log_file = os.path.join(base_path, "font_offsets.txt")

title = "/* 请复制以下代码到你的单片机头文件中 */"

current_offset = 0

with open(log_file, 'w', encoding='utf-8') as f_log:
    print(title, file = f_log)
    print(title)

with open(output_file, "wb") as f_out:
    for file_name in files:
        full_path = os.path.join(base_path, file_name)
        if not os.path.exists(full_path):
            continue
        
        # 4KB 扇区对齐（W25Q的擦除单位是4KB，对齐了以后万一要单独更新某个字库极方便）
        if current_offset % 4096 != 0:
            padding_size = 4096 - (current_offset % 4096)
            f_out.write(b'\xFF' * padding_size)
            current_offset += padding_size
            
        log_msg = f"#define FONT_ADDR_{file_name.split('.')[0]:<10} 0x{current_offset:06X}"

        with open(log_file, "a") as f_log:
            print(log_msg, file = f_log)

        print(log_msg)
        
        with open(full_path, "rb") as f_in:
            data = f_in.read()
            f_out.write(data)
            current_offset += len(data)

print(f"\n合并完成！总大小: {current_offset} 字节 (约 {current_offset/1024/1024:.2f} MB)")