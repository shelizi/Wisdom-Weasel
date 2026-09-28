"""把 CKIP 的繁中 GPT-2（ckiplab/gpt2-*-chinese）轉成 llama.cpp 的 GGUF。

CKIP 的 GPT-2 用 bert-base-chinese 的 WordPiece 詞表（一個漢字一個 token），llama.cpp 的
GPT-2 轉換器只會 BPE：這裡沿用轉換器的 GPT2Model（權重），詞表改用 BertModel 的 WPM 寫法，
並關掉 WPM 預設在句尾加的 [SEP]（評分時句尾不能多一個 token）。句首的 [CLS] 保留（訓練時就有）。

用法：
  python convert_ckip_gpt2.py <llama.cpp 原始碼資料夾> <ckiplab/gpt2-base-chinese 資料夾> <out.gguf> [f16|q8_0]
模型授權是 GPL-3.0（和本專案相同）。
"""
import sys

llama_dir, model_dir, out, outtype = sys.argv[1], sys.argv[2], sys.argv[3], (sys.argv[4] if len(sys.argv) > 4 else 'f16')
sys.path.insert(0, llama_dir)
sys.path.insert(0, llama_dir + '/gguf-py')

import convert_hf_to_gguf as convert  # noqa: E402
from conversion.bert import BertModel  # noqa: E402
from conversion.gpt2 import GPT2Model  # noqa: E402


def set_vocab(self):
    BertModel.set_vocab(self)
    self.gguf_writer.add_add_bos_token(True)
    self.gguf_writer.add_add_eos_token(False)


_modify = GPT2Model.modify_tensors


def modify_tensors(self, data_torch, name, bid):
    # 舊版 transformers 存下來的注意力遮罩 buffer，不是權重
    if name.endswith(('.attn.bias', '.attn.masked_bias')):
        return iter(())
    return _modify(self, data_torch, name, bid)


_pre = GPT2Model.get_vocab_base_pre


def get_vocab_base_pre(self, tokenizer):
    # WPM 詞表不用 BPE 的 pre-tokenizer（llama.cpp 的 WPM 自己切字）；認不得的雜湊當 default
    try:
        return _pre(self, tokenizer)
    except NotImplementedError:
        return 'default'


GPT2Model.set_vocab = set_vocab
GPT2Model.modify_tensors = modify_tensors
GPT2Model.get_vocab_base_pre = get_vocab_base_pre
sys.argv = ['convert_hf_to_gguf.py', model_dir, '--outfile', out, '--outtype', outtype]
convert.main()
