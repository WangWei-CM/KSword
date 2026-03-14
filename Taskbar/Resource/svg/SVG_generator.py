from PIL import Image
import os

# 设置输入和输出目录
input_dir = 'path/to/your/icons'  # 替换为你的图标文件夹路径
output_image_path = 'path/to/output/combined_image.png'  # 替换为你希望保存合并图片的路径

# 设置缩略图的大小
thumbnail_size = (100, 100)  # 你可以根据需要调整这个大小

# 获取所有 .png 文件
png_files = [f for f in os.listdir(input_dir) if f.endswith('.png')]

# 计算合并图片的大小
num_files = len(png_files)
num_cols = 10  # 每行显示的缩略图数量
num_rows = (num_files + num_cols - 1) // num_cols

# 创建一个空白的图片来存放所有缩略图
combined_image = Image.new('RGB', (num_cols * thumbnail_size[0], num_rows * thumbnail_size[1]), (255, 255, 255))

# 遍历所有 .png 文件，生成缩略图并粘贴到合并图片中
for i, filename in enumerate(png_files):
    file_path = os.path.join(input_dir, filename)
    with Image.open(file_path) as img:
        img.thumbnail(thumbnail_size)
        x = (i % num_cols) * thumbnail_size[0]
        y = (i // num_cols) * thumbnail_size[1]
        combined_image.paste(img, (x, y))

# 保存合并图片
combined_image.save(output_image_path)

print(f"Combined image saved to {output_image_path}")
