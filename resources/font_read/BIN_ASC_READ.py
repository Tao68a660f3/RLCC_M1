
class ASC_bin_reader:
    def __init__(self, bin_path):
        self.is_ready = False
        self.font_data = None
        self.height = 0
        self.width = 0
        self.bpc = 0
        self.config = 0
        self.is_vert_scan = False
        self.is_lsb = False
        self.stride = 0
        self.width_table = []
        self._load_bin(bin_path)

    def _load_bin(self, bin_path):
        try:
            if not os.path.exists(bin_path): return
            with open(bin_path, "rb") as f:
                magic = f.read(4)
                if magic != b'FONT': return
                
                self.height = struct.unpack('<B', f.read(1))[0]
                self.width = struct.unpack('<B', f.read(1))[0]
                self.bpc = struct.unpack('<H', f.read(2))[0]
                self.config = struct.unpack('<B', f.read(1))[0]
                f.read(7)  # 跳过保留位
                
                self.is_vert_scan = (self.config & 0x02) != 0  # Bit 1
                self.is_lsb = (self.config & 0x04) != 0        # Bit 2
                
                if self.is_vert_scan:
                    self.stride = (self.height + 7) // 8
                else:
                    self.stride = (self.width + 7) // 8
                
                self.width_table = list(f.read(256))
                self.font_data = f.read()
                self.is_ready = True
        except Exception as e:
            print(f"Load Error: {e}")

    def get_text_bmp(self, asc, y_offset=0, *not_used_argv) -> Image.Image:
        if not self.is_ready or not asc:
            return Image.new("1", (10, 10), 0)

        ascii_code = ord(asc[0]) if isinstance(asc, str) else int(asc)
        char_w = self.width_table[ascii_code]
        
        start_offset = ascii_code * self.bpc
        char_data = self.font_data[start_offset:start_offset + self.bpc]
        
        img = Image.new("1", (self.width, self.height), 0)
        pixels = img.load()
        
        main_limit = self.width if self.is_vert_scan else self.height
        sub_limit = self.height if self.is_vert_scan else self.width
        
        for m in range(main_limit):
            for s in range(sub_limit):
                byte_pos = m * self.stride + (s // 8)
                bit_pos = (s % 8) if self.is_lsb else (7 - (s % 8))
                
                if byte_pos < len(char_data):
                    if (char_data[byte_pos] >> bit_pos) & 1:
                        res_x = m if self.is_vert_scan else s
                        res_y = s if self.is_vert_scan else m
                        
                        if res_x < self.width and res_y < self.height:
                            pixels[res_x, res_y] = 1
        
        if char_w < self.width:
            img = img.crop((0, 0, char_w, self.height))

        ext_w = 0
        if not self.config & 0x01:
            ext_w = 1

        img = img.crop((0, y_offset, img.width + ext_w, y_offset + img.height))
        
        return img
    