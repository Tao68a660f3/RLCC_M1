    // 复刻版 GBK 5区排列逻辑
        public string Name => "GBK_Custom_5Zones";

        public IEnumerable<ushort> GetEncodingStream()
        {
            // 1. 0xA1A1 - 0xA9FE (846字)
            // 逻辑正确：(0xA9-0xA1+1) * 94 = 846
            for (int h = 0xA1; h <= 0xA9; h++)
                for (int l = 0xA1; l <= 0xFE; l++)
                    yield return (ushort)((h << 8) | l);

            // 2. GBK扩充5区 (194字): 0xA840 - 0xA9A0
            // 修正：终点改为 0xA0，不跳过 0x7F。每行 (0xA0-0x40+1) = 97字，两行共 194
            for (int h = 0xA8; h <= 0xA9; h++)
                for (int l = 0x40; l <= 0xA0; l++)
                    yield return (ushort)((h << 8) | l);

            // 3. 0xB0A1 - 0xF7FE (6768字)
            // 逻辑正确：(0xF7-0xB0+1) * 94 = 72 * 94 = 6768
            for (int h = 0xB0; h <= 0xF7; h++)
                for (int l = 0xA1; l <= 0xFE; l++)
                    yield return (ushort)((h << 8) | l);

            // 4. GBK扩充3区 (6112字): 0x8140 - 0xA0FE
            // 修正：终点改为 0xFE，不跳过 0x7F。每行 (0xFE-0x40+1) = 191字，32行共 6112
            for (int h = 0x81; h <= 0xA0; h++)
                for (int l = 0x40; l <= 0xFE; l++)
                    yield return (ushort)((h << 8) | l);

            // 5. GBK扩充4区 (8148字): 0xAA40 - 0xFEA0
            // 修正：将原有的 5 和 6 整合，终点改为 0xFE，低位到 0xA0。
            // 计算：(0xFE-0xAA+1) = 85行。每行 (0xA0-0x40+1) = 97字。85 * 97 = 8245? 
            // 不对，文档说扩充4区到 0xFE4F 结束。我们还是分两段写，确保总数 22084。

            // 5a. 0xAA40 - 0xFDA0 (8148字)
            // (0xFD-0xAA+1) = 84行。84 * 97 = 8148字。
            for (int h = 0xAA; h <= 0xFD; h++)
                for (int l = 0x40; l <= 0xA0; l++)
                    yield return (ushort)((h << 8) | l);

            // 6. 补丁区 (16字): 0xFE40 - 0xFE4F
            // (0x4F-0x40+1) = 16字。
            for (int l = 0x40; l <= 0x4F; l++)
                yield return (ushort)(0xFE00 | l);
        }
        
        public int GetIndexByCode(ushort code)
        {
            byte h = (byte)(code >> 8);
            byte l = (byte)(code & 0xFF);

            // 1. 0xA1A1 - 0xA9FE (846字)
            if (h >= 0xA1 && h <= 0xA9 && l >= 0xA1 && l <= 0xFE)
                return (h - 0xA1) * 94 + (l - 0xA1);

            int baseIndex = 846;

            // 2. GBK扩充5区 (194字): 0xA840 - 0xA9A0 (每行97字)
            if (h >= 0xA8 && h <= 0xA9 && l >= 0x40 && l <= 0xA0)
            {
                return baseIndex + (h - 0xA8) * 97 + (l - 0x40);
            }

            baseIndex += 194;

            // 3. 0xB0A1 - 0xF7FE (6768字)
            if (h >= 0xB0 && h <= 0xF7 && l >= 0xA1 && l <= 0xFE)
                return baseIndex + (h - 0xB0) * 94 + (l - 0xA1);

            baseIndex += 6768;

            // 4. GBK扩充3区 (6112字): 0x8140 - 0xA0FE (每行191字)
            if (h >= 0x81 && h <= 0xA0 && l >= 0x40 && l <= 0xFE)
            {
                return baseIndex + (h - 0x81) * 191 + (l - 0x40);
            }

            baseIndex += 6112;

            // 5. GBK扩充4区 (8148字): 0xAA40 - 0xFDA0 (每行97字)
            if (h >= 0xAA && h <= 0xFD && l >= 0x40 && l <= 0xA0)
            {
                return baseIndex + (h - 0xAA) * 97 + (l - 0x40);
            }

            baseIndex += 8148;

            // 6. 补丁区 (16字): 0xFE40 - 0xFE4F
            if (h == 0xFE && l >= 0x40 && l <= 0x4F)
                return baseIndex + (l - 0x40);

            return -1;
        }

        private int CalculateBytesSize()
        {
            int w = (int)numW.Value;
            int h = (int)numH.Value;
            if ((ScanMode)cmbScan.SelectedItem == ScanMode.Horizontal)
                return (w + 7) / 8 * h;
            else
                return (h + 7) / 8 * w;
        }
        
        private void RunInspect()
        {
            if (_fontData == null || string.IsNullOrEmpty(txtInput.Text))
                return;

            int bytesPerChar = CalculateBytesSize();
            int w = (int)numW.Value;
            int h = (int)numH.Value;
            int zoom = (int)numZoom.Value;

            IEncodingProvider provider = cmbEncoding.SelectedIndex == 0 ? new GbkCustomProvider() : new Gb2312Provider();

            List<byte[]> glyphs = new List<byte[]>();
            StringBuilder sbCode = new StringBuilder();

            // 多字符处理
            foreach (char c in txtInput.Text)
            {
                byte[] bytes = Encoding.GetEncoding("GBK").GetBytes(c.ToString());
                ushort code = bytes.Length >= 2 ? (ushort)(bytes[0] << 8 | bytes[1]) : bytes[0];

                int index = provider.GetIndexByCode(code);
                long offset = (long)index * bytesPerChar;

                if (index != -1 && offset + bytesPerChar <= _fontData.Length)
                {
                    byte[] g = new byte[bytesPerChar];
                    Array.Copy(_fontData, offset, g, 0, bytesPerChar);
                    glyphs.Add(g);
                    sbCode.AppendLine($"// '{c}' Code:0x{code:X4} Index:{index} Offset:0x{offset:X}");
                }
            }

            if (glyphs.Count > 0)
            {
                RenderMultiToCanvas(glyphs, w, h, zoom);
                txtCode.Text = sbCode.ToString();
                lblOffsetInfo.Text = $"已显示 {glyphs.Count} 个字符";
            }
        }

        private void RenderMultiToCanvas(List<byte[]> glyphs, int w, int h, int zoom)
        {
            int spacing = zoom * 0; // 字符间距设为 0 个 Zoom 单位
            int totalW = glyphs.Count * (w * zoom + spacing);
            int totalH = h * zoom;

            Bitmap bmp = new Bitmap(totalW, totalH);
            using (Graphics g = Graphics.FromImage(bmp))
            {
                g.Clear(Color.FromArgb(0, 0, 0));
                for (int i = 0; i < glyphs.Count; i++)
                {
                    int charOffsetX = i * (w * zoom + spacing);
                    byte[] data = glyphs[i];

                    for (int y = 0; y < h; y++)
                    {
                        for (int x = 0; x < w; x++)
                        {
                            if (GetBitFromData(data, x, y, w, h))
                            {
                                g.FillRectangle(Brushes.Lime, charOffsetX + x * zoom, y * zoom, zoom - 1, zoom - 1);
                            }
                            else
                            {
                                g.FillRectangle(new SolidBrush(Color.FromArgb(30, 30, 30)), charOffsetX + x * zoom, y * zoom, zoom - 1, zoom - 1);
                            }
                        }
                    }
                }
            }
            var old = picInspect.Image;
            picInspect.Image = bmp;
            old?.Dispose();
        }

