# Tools

## ZIP 测试数据生成

`generate_zip_test_data.py` 用于生成 EveryZip 压测数据。默认规模为 `10000` 个 ZIP、`5300000` 个内部文件，中文命名比例约 `30%`，输出到 `E:\EveryZipTestData`。

常规生成：

```powershell
python .\tools\generate_zip_test_data.py `
  --output-root E:\EveryZipTestData `
  --zip-count 10000 `
  --entry-count 5300000 `
  --chinese-ratio 0.30 `
  --zip-chinese-ratio 0.30 `
  --compression deflate `
  --compresslevel 6 `
  --jobs 1 `
  --max-total-bytes 4294967296 `
  --seed 42 `
  --clean `
  --report-json E:\EveryZipTestData\report.json
```

快速生成：

```powershell
python .\tools\generate_zip_test_data.py `
  --output-root E:\EveryZipTestData `
  --zip-count 10000 `
  --entry-count 5300000 `
  --compression store `
  --jobs 4 `
  --clean `
  --report-json E:\EveryZipTestData\report.json
```

小样本预览：

```powershell
python .\tools\generate_zip_test_data.py `
  --output-root E:\EveryZipTestDataPreview100 `
  --zip-count 100 `
  --entry-count 53000 `
  --compression store `
  --jobs 4 `
  --clean `
  --report-json E:\EveryZipTestDataPreview100\report.json
```

校验已有数据：

```powershell
python .\tools\generate_zip_test_data.py `
  --output-root E:\EveryZipTestData `
  --verify-only
```

说明：

- ZIP 文件会分布在输出目录下的少量目录层级中。
- ZIP 内部文件也会带少量目录层级，模拟真实工程目录。
- 文件名和 ZIP 名默认约 `30%` 使用中文。
- 文件后缀一律使用英文扩展名，避免 `.脚本`、`.配置` 这类后缀。
- `--compression store --jobs 4` 速度最快，体积会比 deflate 稍大。
- `--compression deflate --compresslevel 1 --jobs 4` 适合兼顾压缩和速度。
- `--clean` 会删除并重建输出目录，日志不要重定向到输出目录内部。
