# 主机侧回归（无需开发板）

```bash
gcc -I<cjson_dir> -I../../components/kaleido_memory/include \
    -o /tmp/km km_host_test.c <cjson.c> \
    ../../components/kaleido_memory/src/kaleido_memory.c -lm
/tmp/km   # KM_HOST_TEST,PASS
```

- km_host_test.c：kaleido_memory 真实文件 IO 回归（append/tag/keyword/
  时间窗排序/core/备份/seq 重载，8 断言）——2026-10-25 PASS
- kp_host_test.c：kaleido_persona 解析/校验/注册表回归（合法包字段、
  无兜底音色拒装、路径越界拒装、install 去重/激活/读取）——2026-10-25 PASS
