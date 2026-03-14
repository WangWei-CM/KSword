import os
import chardet

def detect_encoding(file_path):
    """检测文件编码"""
    with open(file_path, 'rb') as f:
        # 读取前10KB数据用于编码检测，提高准确性
        raw_data = f.read(10240)
        result = chardet.detect(raw_data)
        return result['encoding']

def convert_to_utf8(file_path):
    """将文件转换为UTF-8编码"""
    try:
        # 检测当前编码
        encoding = detect_encoding(file_path)
        if not encoding:
            print(f"⚠️ 无法检测 {file_path} 的编码，跳过处理")
            return False

        # 如果已经是UTF-8则跳过
        if encoding.lower() in ['utf-8', 'utf-8-sig']:
            print(f"ℹ️ {file_path} 已是UTF-8编码，无需转换")
            return True

        # 读取文件内容
        with open(file_path, 'r', encoding=encoding, errors='replace') as f:
            content = f.read()

        # 写入为UTF-8编码（无BOM）
        with open(file_path, 'w', encoding='utf-8') as f:
            f.write(content)

        print(f"✅ 已转换 {file_path} 从 {encoding} 到 UTF-8")
        return True

    except Exception as e:
        print(f"❌ 处理 {file_path} 时出错: {str(e)}")
        return False

def main():
    # 获取当前目录
    current_dir = os.getcwd()
    print(f"📂 处理目录: {current_dir}")

    # 筛选出.h和.cpp文件（不包含子文件夹）
    for filename in os.listdir(current_dir):
        file_path = os.path.join(current_dir, filename)
        # 只处理文件，不处理文件夹
        if os.path.isfile(file_path):
            # 检查文件后缀
            if filename.lower().endswith(('.h', '.cpp')):
                convert_to_utf8(file_path)

    print("🎉 处理完成")

if __name__ == "__main__":
    # 检查是否安装了chardet库
    try:
        import chardet
    except ImportError:
        print("❌ 未找到chardet库，请先安装：pip install chardet")
        exit(1)
    
    main()