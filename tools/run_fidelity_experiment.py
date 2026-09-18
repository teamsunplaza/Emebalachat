# tools/run_fidelity_experiment.py
# Generates sentences for 9 language pairs, builds manifests for Condition A & B,
# runs inference via fidelity_probe.exe, and aggregates results to fidelity_experiment_raw.json.

import os
import sys
import subprocess
import json

PAIRS_DATA = [
    {
        "pair": "KO->EN",
        "src_lang": "Korean",
        "tgt_lang": "English",
        "is_zh_target": False,
        "items": [
            {
                "id": "KO-EN-01",
                "genre": "Conversational",
                "text": "오늘 저녁에 퇴근하고 친구들이랑 매운 떡볶이 먹으러 갈 거예요."
            },
            {
                "id": "KO-EN-02",
                "genre": "Colloquial / Casual",
                "text": "야 주말에 심심한데 우리 같이 영화나 보러 갈래?"
            },
            {
                "id": "KO-EN-03",
                "genre": "Game / Chat",
                "text": "상대 팀 정글러가 지금 바텀 쪽으로 숨어서 돌아가고 있어요."
            },
            {
                "id": "KO-EN-04",
                "genre": "Game / Chat",
                "text": "이번 판에 궁극기 먼저 쓰지 말고 타이밍 기다려봐."
            },
            {
                "id": "KO-EN-05",
                "genre": "Technical",
                "text": "데이터베이스 연결 타임아웃 오류가 발생하여 서버를 즉시 재부팅했습니다."
            },
            {
                "id": "KO-EN-06",
                "genre": "Technical",
                "text": "새 업데이트 패치를 적용하려면 반드시 관리자 권한으로 실행해야 합니다."
            },
            {
                "id": "KO-EN-07",
                "genre": "Idiomatic / Embellishment-prone",
                "text": "그 친구는 원래 발이 넓어서 아는 사람이 진짜 많아."
            },
            {
                "id": "KO-EN-08",
                "genre": "Colloquial",
                "text": "어제 잠을 한숨도 못 자서 하루 종일 피곤해 죽겠어."
            },
            {
                "id": "KO-EN-09",
                "genre": "Instructional / Formal",
                "text": "다음 주 회의 전까지 검토 의견서를 작성해 제출해 주십시오."
            },
            {
                "id": "KO-EN-10",
                "genre": "Colloquial / Slang",
                "text": "저 사람은 맨날 말만 번지르르하고 약속은 하나도 안 지켜."
            }
        ]
    },
    {
        "pair": "ES->EN",
        "src_lang": "Spanish",
        "tgt_lang": "English",
        "is_zh_target": False,
        "items": [
            {
                "id": "ES-EN-01",
                "genre": "Conversational",
                "text": "Vamos a comer una paella deliciosa con toda la familia mañana."
            },
            {
                "id": "ES-EN-02",
                "genre": "Colloquial",
                "text": "Oye, ¿tienes ganas de salir a tomar algo este fin de semana?"
            },
            {
                "id": "ES-EN-03",
                "genre": "Game / Chat",
                "text": "El enemigo está escondido detrás del muro esperando para atacarnos."
            },
            {
                "id": "ES-EN-04",
                "genre": "Game / Chat",
                "text": "No dispares todavía, espera a que todos estemos en posición."
            },
            {
                "id": "ES-EN-05",
                "genre": "Technical",
                "text": "El servidor de prueba no responde debido a un fallo inesperado."
            },
            {
                "id": "ES-EN-06",
                "genre": "Technical",
                "text": "Debe reiniciar el enrutador antes de configurar la nueva dirección IP."
            },
            {
                "id": "ES-EN-07",
                "genre": "Idiomatic",
                "text": "Mi abuela siempre dice que más vale tarde que nunca jamás."
            },
            {
                "id": "ES-EN-08",
                "genre": "Colloquial",
                "text": "Estoy muy cansado porque anoche me quedé trabajando hasta muy tarde."
            },
            {
                "id": "ES-EN-09",
                "genre": "Instructional",
                "text": "Por favor, envíe el informe financiero firmado antes del próximo viernes."
            },
            {
                "id": "ES-EN-10",
                "genre": "Game / Colloquial",
                "text": "¡Cuidado con esa trampa peligrosa, no caigas en su emboscada!"
            }
        ]
    },
    {
        "pair": "EN->ES",
        "src_lang": "English",
        "tgt_lang": "Spanish",
        "is_zh_target": False,
        "items": [
            {
                "id": "EN-ES-01",
                "genre": "Conversational",
                "text": "We should definitely grab a quick coffee before the morning presentation."
            },
            {
                "id": "EN-ES-02",
                "genre": "Colloquial",
                "text": "Hey, let's hang out at the beach this coming Saturday afternoon."
            },
            {
                "id": "EN-ES-03",
                "genre": "Game / Chat",
                "text": "Don't push mid alone without waiting for the whole team."
            },
            {
                "id": "EN-ES-04",
                "genre": "Game / Chat",
                "text": "Watch out for the sniper hiding on top of the tower."
            },
            {
                "id": "EN-ES-05",
                "genre": "Technical",
                "text": "Please restart the background service after updating the network configuration file."
            },
            {
                "id": "EN-ES-06",
                "genre": "Technical",
                "text": "The firewall unexpectedly blocked incoming traffic on port eighty eighty today."
            },
            {
                "id": "EN-ES-07",
                "genre": "Idiomatic",
                "text": "It is completely useless to cry over spilled milk right now."
            },
            {
                "id": "EN-ES-08",
                "genre": "Colloquial",
                "text": "I am feeling completely exhausted after working overtime the entire week."
            },
            {
                "id": "EN-ES-09",
                "genre": "Instructional",
                "text": "Make sure to save all changes before closing this software window."
            },
            {
                "id": "EN-ES-10",
                "genre": "Colloquial",
                "text": "His hilarious story made everyone burst into laughter during dinner tonight."
            }
        ]
    },
    {
        "pair": "HI->EN",
        "src_lang": "Hindi",
        "tgt_lang": "English",
        "is_zh_target": False,
        "items": [
            {
                "id": "HI-EN-01",
                "genre": "Conversational",
                "text": "आज शाम को काम खत्म करने के बाद हम चाय पीने जाएंगे।"
            },
            {
                "id": "HI-EN-02",
                "genre": "Colloquial",
                "text": "अरे दोस्त, क्या तुम इस सप्ताहांत मेरे साथ घूमने चलना चाहोगे?"
            },
            {
                "id": "HI-EN-03",
                "genre": "Game / Chat",
                "text": "दुश्मन खिलाड़ी इमारत के पीछे छिपकर हमारे आने का इंतजार कर रहा है।"
            },
            {
                "id": "HI-EN-04",
                "genre": "Game / Chat",
                "text": "अभी गोली मत चलाना, पहले पूरी टीम को एक साथ आने दो।"
            },
            {
                "id": "HI-EN-05",
                "genre": "Technical",
                "text": "नेटवर्क कनेक्शन टूटने के कारण सर्वर अचानक बंद हो गया है।"
            },
            {
                "id": "HI-EN-06",
                "genre": "Technical",
                "text": "कृपया नया पासवर्ड सेट करने से पहले सभी सुरक्षा नियम पढ़ें।"
            },
            {
                "id": "HI-EN-07",
                "genre": "Idiomatic",
                "text": "जहां चाह होती है, वहां हमेशा कोई न कोई राह निकल आती है।"
            },
            {
                "id": "HI-EN-08",
                "genre": "Colloquial",
                "text": "मुझे आज बहुत थकान महसूस हो रही है क्योंकि रात को नींद नहीं आई।"
            },
            {
                "id": "HI-EN-09",
                "genre": "Instructional",
                "text": "कल की महत्वपूर्ण बैठक से पहले सभी जरूरी दस्तावेज तैयार कर लीजिए।"
            },
            {
                "id": "HI-EN-10",
                "genre": "Colloquial",
                "text": "वह व्यक्ति हमेशा बड़ी-बड़ी बातें करता है लेकिन कुछ करता नहीं।"
            }
        ]
    },
    {
        "pair": "EN->HI",
        "src_lang": "English",
        "tgt_lang": "Hindi",
        "is_zh_target": False,
        "items": [
            {
                "id": "EN-HI-01",
                "genre": "Conversational",
                "text": "We can have a delicious dinner together after finishing our work."
            },
            {
                "id": "EN-HI-02",
                "genre": "Colloquial",
                "text": "Are you planning to watch the cricket match this Sunday evening?"
            },
            {
                "id": "EN-HI-03",
                "genre": "Game / Chat",
                "text": "Stay behind the shield and do not rush into enemy fire."
            },
            {
                "id": "EN-HI-04",
                "genre": "Game / Chat",
                "text": "Our teammate just dropped his weapon, so go help him now."
            },
            {
                "id": "EN-HI-05",
                "genre": "Technical",
                "text": "The database server failed to synchronize with the secondary backup node."
            },
            {
                "id": "EN-HI-06",
                "genre": "Technical",
                "text": "Please enter your registered mobile number to verify the login attempt."
            },
            {
                "id": "EN-HI-07",
                "genre": "Idiomatic",
                "text": "Actions definitely speak louder than words in every situation in life."
            },
            {
                "id": "EN-HI-08",
                "genre": "Colloquial",
                "text": "I forgot my apartment keys inside and got locked outside today."
            },
            {
                "id": "EN-HI-09",
                "genre": "Instructional",
                "text": "Submit the complete quarterly project report before the final deadline tomorrow."
            },
            {
                "id": "EN-HI-10",
                "genre": "Colloquial",
                "text": "She gave a wonderful speech that inspired everyone in the audience."
            }
        ]
    },
    {
        "pair": "ZH->EN",
        "src_lang": "Chinese Simplified",
        "tgt_lang": "English",
        "is_zh_target": False,
        "items": [
            {
                "id": "ZH-EN-01",
                "genre": "Conversational",
                "text": "今天晚上下班以后，我们一起去吃那家很有名的火锅吧。"
            },
            {
                "id": "ZH-EN-02",
                "genre": "Colloquial",
                "text": "这个周末要是天气好，我们就去郊区露营爬山怎么样？"
            },
            {
                "id": "ZH-EN-03",
                "genre": "Game / Chat",
                "text": "对方刺客正在草丛里埋伏，大家千万不要一个人过去。"
            },
            {
                "id": "ZH-EN-04",
                "genre": "Game / Chat",
                "text": "这把比赛稳扎稳打就行，残血的队友先回城补状态。"
            },
            {
                "id": "ZH-EN-05",
                "genre": "Technical",
                "text": "生产环境服务器因内存溢出突然崩溃，请立即进行重启排查。"
            },
            {
                "id": "ZH-EN-06",
                "genre": "Technical",
                "text": "请在更新配置文件之后，重新启动后台数据处理守护进程。"
            },
            {
                "id": "ZH-EN-07",
                "genre": "Idiomatic",
                "text": "纸是包不住火的，事实真相总有一天会大白于天下。"
            },
            {
                "id": "ZH-EN-08",
                "genre": "Colloquial",
                "text": "昨天晚上熬夜赶项目进度，今天整个人都晕乎乎的。"
            },
            {
                "id": "ZH-EN-09",
                "genre": "Instructional",
                "text": "请在明天上午十点之前，将修改好的方案发送到我的邮箱。"
            },
            {
                "id": "ZH-EN-10",
                "genre": "Colloquial",
                "text": "他这个人做事总是雷声大雨点小，根本靠不住。"
            }
        ]
    },
    {
        "pair": "EN->ZH",
        "src_lang": "English",
        "tgt_lang": "Chinese Simplified",
        "is_zh_target": True,
        "items": [
            {
                "id": "EN-ZH-01",
                "genre": "Conversational",
                "text": "Let us go try that new spicy noodle restaurant tonight together."
            },
            {
                "id": "EN-ZH-02",
                "genre": "Colloquial",
                "text": "Do you want to hang out at the shopping mall tomorrow?"
            },
            {
                "id": "EN-ZH-03",
                "genre": "Game / Chat",
                "text": "Fall back immediately because the opposing team is surrounding our base."
            },
            {
                "id": "EN-ZH-04",
                "genre": "Game / Chat",
                "text": "Save your ultimate ability until the decisive team fight starts today."
            },
            {
                "id": "EN-ZH-05",
                "genre": "Technical",
                "text": "The system administrator disabled remote login access for all standard users."
            },
            {
                "id": "EN-ZH-06",
                "genre": "Technical",
                "text": "An unexpected authentication error occurred while validating the security token certificate."
            },
            {
                "id": "EN-ZH-07",
                "genre": "Idiomatic",
                "text": "Better safe than sorry when dealing with important personal financial records."
            },
            {
                "id": "EN-ZH-08",
                "genre": "Colloquial",
                "text": "I drank too much strong coffee and cannot fall asleep tonight."
            },
            {
                "id": "EN-ZH-09",
                "genre": "Instructional",
                "text": "Please attach the original purchase receipt when requesting a product refund."
            },
            {
                "id": "EN-ZH-10",
                "genre": "Colloquial",
                "text": "His funny joke completely broke the ice during the tense meeting."
            }
        ]
    },
    {
        "pair": "PT->EN",
        "src_lang": "Portuguese",
        "tgt_lang": "English",
        "is_zh_target": False,
        "items": [
            {
                "id": "PT-EN-01",
                "genre": "Conversational",
                "text": "Amanhã vamos almoçar juntos naquele restaurante maravilhoso perto da praia."
            },
            {
                "id": "PT-EN-02",
                "genre": "Colloquial",
                "text": "E aí, você quer dar uma volta no parque neste sábado?"
            },
            {
                "id": "PT-EN-03",
                "genre": "Game / Chat",
                "text": "O adversário está escondido na moita pronto para emboscar nosso time."
            },
            {
                "id": "PT-EN-04",
                "genre": "Game / Chat",
                "text": "Não avance sozinho pelo corredor central, espere pelo apoio agora."
            },
            {
                "id": "PT-EN-05",
                "genre": "Technical",
                "text": "O servidor principal caiu repentinamente após uma falha crítica de hardware."
            },
            {
                "id": "PT-EN-06",
                "genre": "Technical",
                "text": "É necessário atualizar o certificado digital antes de iniciar a transmissão."
            },
            {
                "id": "PT-EN-07",
                "genre": "Idiomatic",
                "text": "De grão em grão a galinha enche o papo sempre."
            },
            {
                "id": "PT-EN-08",
                "genre": "Colloquial",
                "text": "Estou morrendo de sono porque passei a noite inteira estudando."
            },
            {
                "id": "PT-EN-09",
                "genre": "Instructional",
                "text": "Por favor, preencha o formulário eletrônico antes do meio-dia de amanhã."
            },
            {
                "id": "PT-EN-10",
                "genre": "Colloquial",
                "text": "Esse sujeito fala demais e nunca cumpre o que promete aos outros."
            }
        ]
    },
    {
        "pair": "KO->PT",
        "src_lang": "Korean",
        "tgt_lang": "Portuguese",
        "is_zh_target": False,
        "items": [
            {
                "id": "KO-PT-01",
                "genre": "Conversational",
                "text": "이번 주말에 가족들이랑 시원한 바닷가로 당일치기 여행을 가려고 해요."
            },
            {
                "id": "KO-PT-02",
                "genre": "Colloquial",
                "text": "오늘 저녁 메뉴로 삼겹살 구워 먹는 거 어때, 괜찮지?"
            },
            {
                "id": "KO-PT-03",
                "genre": "Game / Chat",
                "text": "지금 적팀이 우리 본진으로 몰려오니까 다들 빠르게 기지로 귀환하세요."
            },
            {
                "id": "KO-PT-04",
                "genre": "Game / Chat",
                "text": "내가 앞에서 적의 공격을 막을 테니 뒤에서 지원 사격해줘."
            },
            {
                "id": "KO-PT-05",
                "genre": "Technical",
                "text": "시스템 보안 점검 작업으로 인해 오늘 밤 자정부터 서비스가 중단됩니다."
            },
            {
                "id": "KO-PT-06",
                "genre": "Technical",
                "text": "최신 소프트웨어 버전을 설치하려면 먼저 이전 버전을 완전히 삭제해야 합니다."
            },
            {
                "id": "KO-PT-07",
                "genre": "Idiomatic",
                "text": "시작이 반이라는 말이 있듯이 용기를 내서 일단 먼저 도전해봐."
            },
            {
                "id": "KO-PT-08",
                "genre": "Colloquial",
                "text": "오늘 너무 바빠서 아침부터 지금까지 물 한 모금도 못 마셨어."
            },
            {
                "id": "KO-PT-09",
                "genre": "Instructional",
                "text": "계약서 사본을 꼼꼼히 확인하신 후 맨 아래에 서명해 주시기 바랍니다."
            },
            {
                "id": "KO-PT-10",
                "genre": "Colloquial",
                "text": "그 사람은 겉으로는 친절한 척하지만 속으로는 무슨 생각을 하는지 몰라."
            }
        ]
    }
]

def make_prompts(pair_info, text):
    src = pair_info["src_lang"]
    tgt = pair_info["tgt_lang"]
    if pair_info["is_zh_target"]:
        # Chinese target branch:
        # Cond A: 将以下[src]文本翻译为[tgt]，注意只需要输出翻译后的结果，不要额外解释：\n\n
        # Cond B: 将以下[src]文本准确翻译为[tgt]，注意只需要输出翻译后的结果，不要额外解释：\n\n
        # In Emebalachat config.cpp: src_native="English", tgt_native="简体中文"
        cond_a = f"将以下{src}文本翻译为简体中文，注意只需要输出翻译后的结果，不要额外解释：\n\n{text}"
        cond_b = f"将以下{src}文本准确翻译为简体中文，注意只需要输出翻译后的结果，不要额外解释：\n\n{text}"
    else:
        # English branch:
        # Cond A: Translate the following [src] text into [tgt], without additional explanation:\n\n
        # Cond B: Translate the following [src] text into [tgt] accurately, without additional explanation:\n\n
        cond_a = f"Translate the following {src} text into {tgt}, without additional explanation:\n\n{text}"
        cond_b = f"Translate the following {src} text into {tgt} accurately, without additional explanation:\n\n{text}"
    return cond_a, cond_b

def main():
    repo_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tools_dir = os.path.join(repo_dir, "tools")
    manifest_path = os.path.join(tools_dir, "fidelity_manifest.txt")
    results_path = os.path.join(tools_dir, "fidelity_results.txt")
    probe_exe = os.path.join(tools_dir, "fidelity_probe.exe")
    
    # Model path: try D: drive first, then C: drive
    model_paths = [
        r"D:\OneDrive\Documents\models\tsmodel\Hy-MT2-1.8B-Q8_0.gguf",
        r"C:\Program Files\Emebalachat\models\Hy-MT2-1.8B-Q8_0.gguf"
    ]
    model_path = None
    for p in model_paths:
        if os.path.exists(p):
            model_path = p
            break
            
    if not model_path:
        print("FATAL: model not found in candidates", model_paths, file=sys.stderr)
        sys.exit(1)
        
    print(f"Using model: {model_path}")
    print(f"Using probe: {probe_exe}")

    # Build manifest
    runs = []
    manifest_lines = []
    for pair_data in PAIRS_DATA:
        for itm in pair_data["items"]:
            cond_a_prompt, cond_b_prompt = make_prompts(pair_data, itm["text"])
            
            # Cond A run
            run_id_a = f"{itm['id']}-A"
            manifest_lines.append(f"@@@RUN id={run_id_a}")
            manifest_lines.append(cond_a_prompt)
            manifest_lines.append("@@@END")
            
            # Cond B run
            run_id_b = f"{itm['id']}-B"
            manifest_lines.append(f"@@@RUN id={run_id_b}")
            manifest_lines.append(cond_b_prompt)
            manifest_lines.append("@@@END")

    with open(manifest_path, "w", encoding="utf-8") as f:
        f.write("\n".join(manifest_lines) + "\n")
        
    print(f"Written manifest with {len(manifest_lines)//3} runs to {manifest_path}")

    # Run inference
    cmd = [
        probe_exe,
        "--model", model_path,
        "--manifest", manifest_path,
        "--out", results_path
    ]
    print("Executing probe...")
    res = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    print("Probe stdout:\n", res.stdout)
    print("Probe stderr:\n", res.stderr)
    if res.returncode != 0:
        print(f"FATAL probe failed with code {res.returncode}", file=sys.stderr)
        sys.exit(res.returncode)

    # Parse results
    results_map = {}
    with open(results_path, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()

    blocks = content.split("@@@RESULT ")
    for b in blocks:
        if not b.strip():
            continue
        lines = b.splitlines()
        first_line = lines[0].strip()
        # id=<id>
        if first_line.startswith("id="):
            rid = first_line[3:]
            # text until @@@END
            text_lines = []
            for l in lines[1:]:
                if l.strip() == "@@@END":
                    break
                text_lines.append(l)
            results_map[rid] = "\n".join(text_lines).strip()

    print(f"Parsed {len(results_map)} outputs from probe.")

    # Structure final JSON
    structured_data = {
        "metadata": {
            "model": "Hy-MT2-1.8B-Q8_0.gguf",
            "temperature": 0.0,
            "decoding": "greedy",
            "total_pairs": len(PAIRS_DATA),
            "sentences_per_pair": 10,
            "total_sentences": len(PAIRS_DATA) * 10,
            "total_inferences": len(PAIRS_DATA) * 10 * 2,
            "condition_A_description": "Without 'accurately' / '准确' in prompt template",
            "condition_B_description": "With 'accurately' / '准确' in prompt template"
        },
        "pairs": []
    }

    total_diff_count = 0
    total_identical_count = 0

    for pair_data in PAIRS_DATA:
        pair_record = {
            "pair": pair_data["pair"],
            "src_lang": pair_data["src_lang"],
            "tgt_lang": pair_data["tgt_lang"],
            "is_zh_target": pair_data["is_zh_target"],
            "items": [],
            "stats": {
                "total": len(pair_data["items"]),
                "identical": 0,
                "different": 0
            }
        }
        for itm in pair_data["items"]:
            cond_a_prompt, cond_b_prompt = make_prompts(pair_data, itm["text"])
            out_a = results_map.get(f"{itm['id']}-A", "")
            out_b = results_map.get(f"{itm['id']}-B", "")
            is_diff = (out_a != out_b)
            if is_diff:
                pair_record["stats"]["different"] += 1
                total_diff_count += 1
            else:
                pair_record["stats"]["identical"] += 1
                total_identical_count += 1

            pair_record["items"].append({
                "id": itm["id"],
                "genre": itm["genre"],
                "src_text": itm["text"],
                "cond_a": {
                    "prompt": cond_a_prompt,
                    "translation": out_a
                },
                "cond_b": {
                    "prompt": cond_b_prompt,
                    "translation": out_b
                },
                "is_different": is_diff
            })
        structured_data["pairs"].append(pair_record)

    structured_data["metadata"]["summary_stats"] = {
        "total_items": len(PAIRS_DATA) * 10,
        "identical_count": total_identical_count,
        "different_count": total_diff_count,
        "difference_ratio": f"{(total_diff_count / (len(PAIRS_DATA)*10)) * 100:.1f}%"
    }

    # Save to requested destination:
    # C:\Users\k1yt\.gemini\antigravity-cli\brain\8e062b9f-7b17-4f99-b763-1207f27687a5\scratch\fidelity_experiment_raw.json
    dest_path = r"C:\Users\k1yt\.gemini\antigravity-cli\brain\8e062b9f-7b17-4f99-b763-1207f27687a5\scratch\fidelity_experiment_raw.json"
    os.makedirs(os.path.dirname(dest_path), exist_ok=True)
    with open(dest_path, "w", encoding="utf-8") as f:
        json.dump(structured_data, f, ensure_ascii=False, indent=2)

    print(f"Successfully written structured experiment raw JSON to: {dest_path}")
    print(f"Summary: {total_identical_count} identical, {total_diff_count} different ({structured_data['metadata']['summary_stats']['difference_ratio']})")

if __name__ == "__main__":
    main()
