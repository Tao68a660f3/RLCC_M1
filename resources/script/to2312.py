import sys
import re
import pyperclip  # 需要安装pyperclip库

def str_to_gb2312_hex(input_str, output_format='default'):
    try:
        # 转换为GB2312字节序列
        gb2312_bytes = input_str.encode('gb2312')
        
        # 输出格式选择
        if output_format == 'c_array':
            # C语言数组格式
            hex_values = ['0x{:02X}'.format(b) for b in gb2312_bytes]
            # c_array = 'const unsigned char code str01[] = {' + ', '.join(hex_values) + ', 0x00};'
            c_array = '{' + ', '.join(hex_values) + ', 0x00};'

            
            print("\nC语言数组格式:")
            print(c_array)
            
            # 复制到剪贴板
            pyperclip.copy(c_array)
            print("(结果已自动复制到剪贴板)\n")
            
            return c_array
        else:
            # 默认格式（原始字节）
            print("原始GB2312编码字节:")
            print(' '.join(['{:02X}'.format(b) for b in gb2312_bytes]))
        
        return gb2312_bytes
    except UnicodeEncodeError:
        print(f"错误: 字符 '{input_str}' 不属于GB2312编码范围")
        return None
    except Exception as e:
        print(f"发生错误: {e}")
        return None

if __name__ == '__main__':
    print("="*50)
    print("GB2312转C语言数组工具")
    print("输入字符串后，将自动生成GB2312编码的C数组并复制到剪贴板")
    print("="*50)
    
    try:
        while True:
            # 获取用户输入
            input_str = input("\n请输入字符串(输入q退出): ")
            
            if input_str.lower() == 'q':
                print("程序退出")
                break
                
            if not input_str:
                print("输入不能为空！")
                continue
                
            # 处理并输出结果
            result = str_to_gb2312_hex(input_str, "c_array")
            
    except KeyboardInterrupt:
        print("\n程序被用户中断")
    except Exception as e:
        print(f"程序发生错误: {e}")