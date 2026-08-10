import sys
import socket
import binascii
import time
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLineEdit, QPushButton, QComboBox, QTextEdit, QLabel, QGroupBox,
    QGridLayout, QMessageBox
)
from PyQt5.QtCore import Qt
from PyQt5.QtGui import QFont, QTextCursor


class UDPPacketSender(QMainWindow):
    # 协议常量
    HEADER = 0xAA
    CMD = 0x16
    
    def __init__(self):
        super().__init__()
        self.udp_socket = None
        self.initUI()
        
    def initUI(self):
        self.setWindowTitle("UDP数据包发送工具")
        self.setGeometry(300, 300, 850, 600)
        
        # 主窗口部件
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        main_layout = QVBoxLayout(central_widget)
        
        # ========== 目标地址设置区域 ==========
        addr_group = QGroupBox("UDP目标设置")
        addr_layout = QHBoxLayout()
        
        addr_layout.addWidget(QLabel("IP地址:"))
        self.ip_input = QLineEdit("127.0.0.1")
        self.ip_input.setPlaceholderText("请输入IPv4地址")
        addr_layout.addWidget(self.ip_input)
        
        addr_layout.addWidget(QLabel("端口:"))
        self.port_input = QLineEdit("8888")
        self.port_input.setPlaceholderText("请输入端口号")
        self.port_input.setFixedWidth(100)
        addr_layout.addWidget(self.port_input)
        
        addr_layout.addStretch()
        addr_group.setLayout(addr_layout)
        main_layout.addWidget(addr_group)
        
        # ========== 编码选择 ==========
        encode_layout = QHBoxLayout()
        encode_layout.addWidget(QLabel("编码方式:"))
        self.encode_combo = QComboBox()
        self.encode_combo.addItems(["UTF-8", "GBK"])
        encode_layout.addWidget(self.encode_combo)
        encode_layout.addStretch()
        main_layout.addLayout(encode_layout)
        
        # ========== 输入和发送区域 ==========
        send_layout = QHBoxLayout()
        self.text_input = QLineEdit()
        self.text_input.setPlaceholderText("请输入要发送的文本，按回车键发送")
        self.text_input.returnPressed.connect(self.send_packet)
        send_layout.addWidget(self.text_input)
        
        self.send_btn = QPushButton("发送")
        self.send_btn.setFixedWidth(100)
        self.send_btn.clicked.connect(self.send_packet)
        send_layout.addWidget(self.send_btn)
        main_layout.addLayout(send_layout)
        
        # ========== 日志区域 ==========
        log_group = QGroupBox("日志")
        log_layout = QVBoxLayout()
        
        self.log_text = QTextEdit()
        self.log_text.setReadOnly(True)
        self.log_text.setFont(QFont("Consolas", 10))
        log_layout.addWidget(self.log_text)
        
        # 清空日志按钮
        clear_layout = QHBoxLayout()
        clear_layout.addStretch()
        self.clear_btn = QPushButton("清空日志")
        self.clear_btn.clicked.connect(self.clear_log)
        clear_layout.addWidget(self.clear_btn)
        log_layout.addLayout(clear_layout)
        
        log_group.setLayout(log_layout)
        main_layout.addWidget(log_group)
        
        # 设置窗口最小大小
        self.setMinimumSize(700, 500)
    
    def build_packet(self, payload):
        """构建数据包：AA [Cmd] [LenH] [LenL] [Payload] [Check]"""
        length = len(payload)
        frame = bytearray()
        frame.append(self.HEADER)          # AA
        frame.append(self.CMD)             # Cmd
        frame.append((length >> 8) & 0xFF) # LenH
        frame.append(length & 0xFF)        # LenL
        frame.extend(payload)              # Payload
        
        # 全帧异或校验：从AA到Payload所有字节
        check = 0
        for b in frame:
            check ^= b
        frame.append(check)                # Check
        
        return bytes(frame)
    
    def get_timestamp(self):
        """获取当前时间戳字符串"""
        return time.strftime("%H:%M:%S")
    
    def log_message(self, hex_str, text_preview, status="成功"):
        """在日志区域添加一条记录"""
        timestamp = self.get_timestamp()
        log_entry = (
            f"[{timestamp}] 发送{status}:\n"
            f"  HEX: {hex_str}\n"
            f"  TXT: {text_preview}\n"
            f"{'-' * 50}\n"
        )
        self.log_text.append(log_entry)
        # 自动滚动到底部
        cursor = self.log_text.textCursor()
        cursor.movePosition(QTextCursor.End)
        self.log_text.setTextCursor(cursor)
    
    def send_packet(self):
        """发送数据包的主逻辑"""
        # 1. 获取输入文本
        text = self.text_input.text().strip()
        if not text:
            QMessageBox.warning(self, "警告", "请输入要发送的文本！")
            return
        
        # 2. 获取目标地址和端口
        ip = self.ip_input.text().strip()
        if not ip:
            QMessageBox.warning(self, "警告", "请输入目标IP地址！")
            return
        
        try:
            port = int(self.port_input.text().strip())
            if port < 1 or port > 65535:
                raise ValueError("端口号必须在1-65535之间")
        except ValueError as e:
            QMessageBox.warning(self, "警告", f"无效的端口号: {str(e)}")
            return
        
        # 3. 编码文本
        encoding = self.encode_combo.currentText()
        try:
            if encoding == "UTF-8":
                payload = text.encode('utf-8')
            else:  # GBK
                payload = text.encode('gbk')
        except Exception as e:
            QMessageBox.critical(self, "错误", f"编码失败: {str(e)}")
            return
        
        # 4. 构建数据包
        packet = self.build_packet(payload)
        
        # 5. 生成十六进制预览
        hex_str = ' '.join([f"{b:02X}" for b in packet])
        
        # 6. 发送UDP数据包
        try:
            if self.udp_socket is None:
                self.udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                # 设置超时，防止阻塞
                self.udp_socket.settimeout(1)
            
            self.udp_socket.sendto(packet, (ip, port))
            status = "成功"
        except socket.timeout:
            status = "超时"
            QMessageBox.warning(self, "警告", f"发送超时，目标 {ip}:{port} 无响应")
        except socket.error as e:
            status = f"失败({e})"
            QMessageBox.critical(self, "错误", f"UDP发送失败: {str(e)}")
            # 重置socket以便下次重新创建
            self.udp_socket = None
        except Exception as e:
            status = f"失败({e})"
            QMessageBox.critical(self, "错误", f"未知错误: {str(e)}")
        else:
            # 7. 记录日志（仅成功或超时时记录，错误已在上面处理）
            if status == "成功" or status == "超时":
                self.log_message(hex_str, text, status)
                # 发送成功后清空输入框（可选）
                # self.text_input.clear()
    
    def clear_log(self):
        """清空日志区域"""
        self.log_text.clear()
    
    def closeEvent(self, event):
        """关闭窗口时释放socket资源"""
        if self.udp_socket:
            self.udp_socket.close()
            self.udp_socket = None
        event.accept()


def main():
    app = QApplication(sys.argv)
    window = UDPPacketSender()
    window.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()