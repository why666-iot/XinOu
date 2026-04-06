"""
提示词加载器：启动时读取所有 .txt 提示词文件并缓存

用法：
    from modules import prompt_loader

    # 获取分类器提示词
    classifier_prompt = prompt_loader.get_classifier_prompt()

    # 获取拼接后的系统提示词（base + 场景）
    system_prompt = prompt_loader.get_system_prompt("work/conflict")
"""
import os

# 提示词目录路径
_PROMPTS_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "prompts")

# 合法场景 ID
VALID_SCENES: set[str] = {
    "daily",
    "work/pressure",
    "work/conflict",
    "work/career",
    "emotion/loneliness",
    "emotion/relationship",
    "emotion/anxiety",
    "family/origin",
    "family/expectations",
}

# 内部缓存：{文件相对路径(不含.txt): 文件内容}
_cache: dict[str, str] = {}


def _load_all() -> None:
    """启动时读取所有 .txt 文件到 _cache，并校验必要文件"""
    for root, _dirs, files in os.walk(_PROMPTS_DIR):
        for fname in files:
            if not fname.endswith(".txt"):
                continue
            full_path = os.path.join(root, fname)
            # 转为相对键名：如 "work/pressure"、"base"、"classifier"
            rel = os.path.relpath(full_path, _PROMPTS_DIR).replace("\\", "/")
            key = rel.removesuffix(".txt")
            with open(full_path, encoding="utf-8") as f:
                _cache[key] = f.read().strip()

    # 校验必要文件
    required = {"classifier", "base"} | VALID_SCENES
    missing = required - _cache.keys()
    if missing:
        print(f"[提示词] ⚠ 缺少文件: {', '.join(sorted(missing))}")

    loaded_scenes = VALID_SCENES & _cache.keys()
    print(f"[提示词] 已加载 {len(loaded_scenes)}/{len(VALID_SCENES)} 个场景，共 {len(_cache)} 个文件")


def get_classifier_prompt() -> str:
    """返回分类器系统提示词"""
    return _cache.get("classifier", "")


def get_system_prompt(scene: str, memory_text: str = "") -> str:
    """拼接完整系统提示词：base + 场景 + 记忆（Phase 3 预留）

    参数：
        scene       : 场景 ID（如 "work/conflict"）
        memory_text : 用户记忆文本（Phase 3 接入，当前为空）

    返回：
        拼接后的完整 system prompt
    """
    if scene not in VALID_SCENES:
        scene = "daily"

    base = _cache.get("base", "")
    scene_prompt = _cache.get(scene, "")

    parts = [base, scene_prompt]
    if memory_text:
        parts.append(memory_text)

    return "\n\n".join(parts)


# 模块导入时自动加载
_load_all()
