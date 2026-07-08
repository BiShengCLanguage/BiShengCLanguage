### PR描述

### 修改类型

- [ ] Bugfix 
- [ ] Feature
- [ ] Refactor
- [ ] Docs
- [ ] Others:

### 影响特性

- [ ] SafeZone
- [ ] Ownership
- [ ] Borrow
- [ ] Nullability
- [ ] InitAnalysis
- [ ] Others:

### 影响编译阶段

- [ ] Driver
- [ ] Lex
- [ ] Parser
- [ ] Sema
- [ ] Analysis
- [ ] Others:

### 通用检查清单
- [ ] commit格式检查，包含修改类型/影响特性等
- [ ] 完成了代码格式化
- [ ] 正反向测试用例
- [ ] 不涉及或已完成用户手册更新
- [ ] 评估了存量C代码兼容性

### 高风险场景检查

https://gitcode.com/bisheng_c_language_dep/llvm-project/wiki/BiShengC%20PR%20Review%20Standard.md
- [ ] 非法输入稳定报错，不会 crash
- [ ] ownership /  borrow / nullability 复合场景已覆盖
- [ ] 结构体 / 数组 / （预定义）字符串 复合类型已覆盖
