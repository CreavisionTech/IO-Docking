#!/usr/bin/env python3
"""
IO Docking Board SDK 调用示例

演示如何使用 IODock SDK 控制板卡各外设

运行方式:
    python demo.py COM3          # Windows
    python demo.py /dev/ttyACM0  # Linux
"""

import sys
import time
from iodock import IODock, Level, Direction, PullMode, Edge, Polarity


def print_section(title: str):
    """打印分隔线"""
    print("\n" + "=" * 50)
    print(f"  {title}")
    print("=" * 50 + "\n")


def wait_key(msg: str = "按回车继续..."):
    """等待用户按键"""
    input(msg)


def main():
    # 检查命令行参数
    if len(sys.argv) < 2:
        print(f"用法: {sys.argv[0]} <串口>")
        print(f"示例: {sys.argv[0]} COM3")
        return 1
    
    port = sys.argv[1]
    
    # ========================================================================
    # 1. 创建 IODock 对象并连接
    # ========================================================================
    print_section("1. 连接板卡")
    
    dock = IODock(port)
    
    if not dock.open():
        print(f"错误：无法打开串口 {port}")
        return 1
    print(f"串口已打开：{port}")
    
    # 测试连接
    if dock.ping():
        print("PING 成功，板卡在线")
    else:
        print("PING 失败，请检查连接")
        return 1
    
    # ========================================================================
    # 2. 获取板卡信息
    # ========================================================================
    print_section("2. 板卡信息")
    
    info = dock.get_info()
    for key, val in info.items():
        print(f"  {key} = {val}")
    
    wait_key()
    
    # ========================================================================
    # 3. IO 控制示例
    # ========================================================================
    print_section("3. IO 控制")
    
    # 配置 IO1 为输出
    print("配置 IO1 为输出模式...")
    dock.io_config("IO1", Direction.OUTPUT)
    
    # 输出高电平
    print("IO1 输出高电平（LED 灭）...")
    dock.io_write("IO1", Level.HIGH)
    time.sleep(0.5)
    
    # 输出低电平
    print("IO1 输出低电平（LED 亮）...")
    dock.io_write("IO1", Level.LOW)
    time.sleep(0.5)
    
    # 翻转输出
    print("IO1 翻转输出...")
    dock.io_toggle("IO1")
    time.sleep(0.5)
    
    # 输出脉冲
    print("IO1 输出 200ms 脉冲...")
    dock.io_pulse("IO1", Level.LOW, 200)
    time.sleep(0.3)
    
    # 配置 IO2 为上拉输入
    print("\n配置 IO2 为上拉输入...")
    dock.io_config("IO2", Direction.INPUT, PullMode.PULLUP)
    
    # 读取输入
    level = dock.io_read("IO2")
    print(f"IO2 当前电平：{level.name}")
    
    # 读取全部 IO
    print("\n读取全部 IO 状态：")
    all_levels = dock.io_read_all()
    for i, level in enumerate(all_levels):
        print(f"  IO{i+1}: {level.name}")
    
    wait_key()
    
    # ========================================================================
    # 4. PWM 控制示例
    # ========================================================================
    print_section("4. PWM 控制")
    
    # 配置 PWM1 为 1kHz，50% 占空比
    print("配置 PWM1: 1kHz, 50% 占空比...")
    dock.pwm_config("PWM1", 1000, 50)
    
    # 启动 PWM
    print("启动 PWM1...")
    dock.pwm_start("PWM1")
    time.sleep(1)
    
    # 修改占空比
    print("修改占空比为 25%...")
    dock.pwm_set_duty("PWM1", 25)
    time.sleep(1)
    
    # 呼吸灯效果
    print("运行呼吸灯效果（0→100→0）...")
    dock.pwm_sweep("PWM1", 0, 100, 10, 1)
    time.sleep(3)
    
    # 停止 PWM
    print("停止 PWM1...")
    dock.pwm_stop("PWM1")
    
    # 多路同步示例
    print("\n配置 PWM1 和 PWM2 同频同步...")
    dock.pwm_config("PWM1", 1000, 50)
    dock.pwm_config("PWM2", 1000, 50)
    dock.pwm_sync(["PWM1", "PWM2"])
    time.sleep(2)
    
    # 设置相位偏移
    print("PWM2 相对 PWM1 相位滞后 90°...")
    dock.pwm_set_phase("PWM2", "PWM1", 90)
    time.sleep(2)
    
    dock.pwm_stop("PWM1")
    dock.pwm_stop("PWM2")
    
    wait_key()
    
    # ========================================================================
    # 5. UART 控制示例
    # ========================================================================
    print_section("5. UART 控制")
    
    # 配置 USART1
    print("配置 USART1: 9600 8N1...")
    dock.uart_config("USART1", 9600)
    
    # 发送文本
    print("发送文本 'Hello'...")
    dock.uart_send_text("USART1", "Hello")
    
    # 发送十六进制数据
    print("发送十六进制数据 0x01 0x02 0x03...")
    dock.uart_send_hex("USART1", b'\x01\x02\x03')
    
    # 读取接收缓冲
    print("读取接收缓冲...")
    rx_data = dock.uart_receive("USART1")
    if rx_data:
        print(f"收到 {len(rx_data)} 字节: {rx_data.hex().upper()}")
    else:
        print("接收缓冲为空")
    
    # 启用透传模式
    print("\n启用 USART1 透传模式...")
    dock.uart_stream("USART1", True)
    print("（透传模式下收到的数据会通过事件上报）")
    time.sleep(2)
    dock.uart_stream("USART1", False)
    
    wait_key()
    
    # ========================================================================
    # 6. I2C 控制示例
    # ========================================================================
    print_section("6. I2C 控制")
    
    # 设置 I2C 速率
    print("设置 I2C 速率为 400kHz...")
    dock.i2c_set_speed(400)
    
    # 扫描总线
    print("扫描 I2C 总线...")
    addrs = dock.i2c_scan()
    if addrs:
        print(f"发现 {len(addrs)} 个从机: {[f'0x{a:02X}' for a in addrs]}")
        
        # 尝试读取第一个设备的寄存器 0x00
        dev_addr = addrs[0]
        print(f"\n读取设备 0x{dev_addr:02X} 的寄存器 0x00 (4字节)...")
        data = dock.i2c_read_reg(dev_addr, 0x00, 4)
        if data:
            print(f"读取成功: {data.hex().upper()}")
    else:
        print("未发现 I2C 设备")
    
    wait_key()
    
    # ========================================================================
    # 7. SPI 控制示例
    # ========================================================================
    print_section("7. SPI 控制")
    
    # 配置 SPI
    print("配置 SPI: 1MHz, 模式0, MSB, 8位...")
    dock.spi_config(1000000, 0, "MSB", 8)
    
    # 全双工传输
    print("SPI 全双工传输 0xDEADBEEF...")
    spi_rx = dock.spi_transfer(b'\xDE\xAD\xBE\xEF')
    print(f"收到: {spi_rx.hex().upper()}")
    
    # 手动控制 CS
    print("\n手动拉低 CS...")
    dock.spi_cs("LOW")
    dock.spi_write(b'\x01\x02')
    print("手动拉高 CS...")
    dock.spi_cs("HIGH")
    
    wait_key()
    
    # ========================================================================
    # 8. ADC 控制示例
    # ========================================================================
    print_section("8. ADC 控制")
    
    # 单次读取
    print("读取 ADC0...")
    adc_val = dock.adc_read("ADC0")
    print(f"  原始值: {adc_val.raw}")
    print(f"  电压: {adc_val.millivolts} mV")
    
    # 读取全部通道
    print("\n读取全部 ADC 通道：")
    all_adc = dock.adc_read_all()
    for i, val in enumerate(all_adc):
        print(f"  ADC{i}: {val.raw} ({val.millivolts} mV)")
    
    # 读取温度
    print("\n读取片上温度...")
    temp = dock.adc_read_temp()
    print(f"  温度: {temp:.1f} °C")
    
    # 连续采样示例
    print("\n启动 ADC0 连续采样: 100Hz, 1秒...")
    dock.adc_start_sample("ADC0", 100, 1000)
    print("（采样数据会通过 DATA 事件上报）")
    time.sleep(1.5)
    
    # 设置阈值事件
    print("\n设置 ADC0 阈值事件: 1000mV ~ 2000mV...")
    dock.adc_threshold("ADC0", 1000, 2000, True)
    
    wait_key()
    
    # ========================================================================
    # 9. 组合时序示例
    # ========================================================================
    print_section("9. 组合时序")
    
    # 定义一个简单的序列
    script = "IO IO1 LOW@0; WAIT 200; IO IO1 HIGH@200; WAIT 200; IO IO1 LOW@400"
    
    print(f"定义序列 'blink'：")
    print(f"  {script}")
    dock.seq_define("blink", script)
    
    # 列出已定义序列
    print("\n已定义的序列：")
    seqs = dock.seq_list()
    for name in seqs:
        print(f"  - {name}")
    
    # 执行序列
    print("\n执行序列 'blink' (循环3次)...")
    dock.seq_run("blink", 3)
    time.sleep(3)
    
    # 删除序列
    print("\n删除序列 'blink'...")
    dock.seq_delete("blink")
    
    wait_key()
    
    # ========================================================================
    # 10. 事件监听示例
    # ========================================================================
    print_section("10. 事件监听")
    
    # 注册事件回调
    print("注册事件回调...")
    dock.on_event(lambda event, data: print(f"[事件] {event}: {data}"))
    
    # 启动事件监听
    print("启动事件监听线程...")
    dock.start_event_listener()
    
    # 使能 IO2 边沿事件
    print("使能 IO2 下降沿事件...")
    dock.io_config("IO2", Direction.INPUT, PullMode.PULLUP)
    dock.io_event("IO2", True, Edge.FALLING)
    
    print("\n请手动触发 IO2（连接到 GND），观察事件上报...")
    print("等待 5 秒...")
    time.sleep(5)
    
    # 停止事件监听
    dock.stop_event_listener()
    dock.io_event("IO2", False)
    
    # ========================================================================
    # 11. 清理
    # ========================================================================
    print_section("11. 清理退出")
    
    # 关闭所有输出
    print("关闭所有外设...")
    dock.io_write("IO1", Level.HIGH)  # LED 灭
    dock.pwm_stop("PWM1")
    dock.pwm_stop("PWM2")
    
    # 关闭连接
    dock.close()
    print("连接已关闭")
    
    print("\n演示完成！")
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
