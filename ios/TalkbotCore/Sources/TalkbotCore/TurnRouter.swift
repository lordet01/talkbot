import Foundation

public enum Expect: String, Sendable, Equatable {
    case none, yn, word, number, cont, open, choice
}

public enum Phase: String, Sendable, Equatable {
    case idle, offer, play, ask, hint, done
}

public enum UtterIntent: String, Sendable, Equatable {
    case other, answer, question, correction, reject, help, continueStory, actChange, ambig, safety
}

public enum Beat: String, Sendable, Equatable {
    case follow, greet, answer, ack, hint, clarify, continueStory, comfort, refuse, exit, quizWord
}

/// Deterministic pre-LLM plan. The on-device 3B only sees a compact card + overlay.
public struct TurnPlan: Sendable, Equatable {
    public var intent: UtterIntent
    public var activity: Activity
    public var topic: String
    public var expect: Expect
    public var phase: Phase
    public var hintLevel: Int
    public var expression: Expression
    public var beat: Beat
    public var allowQuestion: Bool
    public var canned: String?
    public var systemPrompt: String
    public var userPrompt: String

    public var skipLLM: Bool { canned != nil }
}

enum TurnRouter {
    struct Snapshot: Equatable {
        var activity: Activity = .free
        var topic = ""
        var hintLevel = 0
        var expression: Expression = .warm
        var expect: Expect = .none
        var phase: Phase = .idle
        var intent: UtterIntent = .other
        var beat: Beat = .follow
        var allowQuestion = false
        var recentQuestions = 0
        var clarifyFails = 0
        var justChanged = false

        var locked: Bool {
            switch activity {
            case .english, .story, .counting, .rolePlay, .meal, .hygiene, .sleep, .hangul, .song, .safe:
                return true
            case .free, .day, .emo:
                return false
            }
        }

        var learning: Bool {
            activity == .english || activity == .counting || activity == .hangul
        }

        mutating func clearPending() {
            expect = .none
            hintLevel = 0
            if phase == .ask || phase == .hint || phase == .offer { phase = .play }
        }

        mutating func leave(_ next: Activity) {
            let prev = activity
            clearPending()
            topic = ""
            activity = next
            phase = next == .free ? .idle : .play
            justChanged = prev != next
        }
    }

    static func decide(state: inout Snapshot, raw: String, age: ChildAge) -> TurnPlan {
        let text = normalize(raw)
        state.justChanged = false

        if text.isEmpty || isGarble(text) {
            state.intent = .ambig
            state.clarifyFails = min(3, state.clarifyFails + 1)
            state.beat = .clarify
            state.allowQuestion = state.clarifyFails < 2
            let canned = state.clarifyFails >= 2 ? "응, 디노 듣고 있어." : nil
            return pack(state, user: text.isEmpty ? "…" : text, age: age, canned: canned)
        }

        if contains(text, kSafe) {
            state.leave(.free)
            state.intent = .safety
            state.beat = .refuse
            state.expression = .thoughtful
            state.allowQuestion = false
            return pack(state, user: text, age: age, canned: "그건 디노랑 안 놀아. 다른 거 하자.")
        }

        if contains(text, kExit) || isBareReject(text) {
            state.leave(.free)
            state.intent = .reject
            state.beat = .exit
            state.expression = .warm
            state.allowQuestion = false
            return pack(state, user: text, age: age)
        }

        if contains(text, kSleep) {
            state.leave(.sleep)
            state.intent = .actChange
            state.beat = .ack
            state.expression = .sleepy
            state.allowQuestion = false
            return pack(state, user: text, age: age)
        }

        if state.expect == .yn {
            if isMostly(text, kYes) {
                state.intent = .answer
                state.phase = .play
                state.expect = .open
                state.beat = .ack
                return pack(state, user: text, age: age)
            }
            if isMostly(text, kNo) || text.hasPrefix("싫어") {
                state.intent = .reject
                state.clearPending()
                if state.phase == .offer { state.leave(.free) }
                state.beat = .exit
                state.expression = .warm
                state.allowQuestion = false
                return pack(state, user: text, age: age)
            }
        }

        if contains(text, kHelp) && (state.learning || state.expect == .word || state.expect == .number
            || state.phase == .ask || state.phase == .hint) {
            state.intent = .help
            state.phase = .hint
            state.hintLevel = min(3, state.hintLevel + 1)
            state.beat = .hint
            state.expression = .thoughtful
            state.allowQuestion = state.hintLevel < 2
            return pack(state, user: text, age: age)
        }

        if state.activity == .story && contains(text, kContinue) {
            state.intent = .continueStory
            state.expect = .cont
            state.beat = .continueStory
            state.allowQuestion = false
            return pack(state, user: text, age: age)
        }

        if contains(text, kCorrection) {
            state.intent = .correction
            state.beat = .answer
            return pack(state, user: text, age: age)
        }

        if state.locked && isEllipsis(text), let topic = extractTopic(text) {
            state.topic = topic
            state.intent = .question
            state.phase = .ask
            state.expect = state.activity == .counting ? .number : .word
            state.beat = state.activity == .english ? .quizWord : .answer
            state.hintLevel = 0
            state.allowQuestion = false
            return pack(state, user: text, age: age)
        }

        if contains(text, kDistress) {
            state.beat = .comfort
            state.expression = .comforting
            state.allowQuestion = state.recentQuestions < 1
            if state.locked {
                state.intent = .other
            } else {
                state.leave(.emo)
                state.intent = .actChange
            }
            return pack(state, user: text, age: age)
        }

        if let want = detectActivity(text) {
            let strong = contains(text, kStrongSwitch)
            if state.locked && want != state.activity && !strong {
                state.intent = .other
                state.beat = .follow
                return pack(state, user: text, age: age)
            }
            if want != state.activity {
                state.leave(want)
                state.intent = .actChange
                state.beat = entryBeat(want)
                if want == .story { state.expect = .cont }
                if want == .english {
                    state.expect = .open
                    state.phase = .play
                }
                if let topic = extractTopic(text), !isCommandResidue(topic) {
                    state.topic = topic
                }
                return pack(state, user: text, age: age)
            }
        }

        if isMostly(text, kYes) && state.expect == .none {
            state.intent = .answer
            state.beat = .ack
            return pack(state, user: text, age: age)
        }

        if isGreeting(text) && !state.locked {
            if state.activity != .day { state.leave(.day) }
            state.intent = .actChange
            state.beat = .greet
            state.expression = .delighted
            return pack(state, user: text, age: age)
        }

        state.intent = text.count <= 2 ? .ambig : .other
        state.beat = state.intent == .ambig ? .clarify : .follow
        if state.intent != .ambig, !isMostly(text, kYes), !contains(text, kContinue) {
            if let topic = extractTopic(text), !isCommandResidue(topic) {
                state.topic = topic
            } else {
                state.topic = String(text.prefix(24))
            }
        }
        if state.intent == .ambig {
            state.clarifyFails = min(3, state.clarifyFails + 1)
        } else {
            state.clarifyFails = 0
        }
        state.allowQuestion = state.recentQuestions < 2 && state.beat == .follow
        return pack(state, user: text, age: age)
    }

    static func afterReply(_ state: inout Snapshot, spoken: String) {
        if spoken.contains("?") {
            state.recentQuestions = min(7, state.recentQuestions + 1)
        } else if state.recentQuestions > 0 {
            state.recentQuestions -= 1
        }
        if contains(spoken, ["속상", "괜찮아", "무서웠", "옆에 있"]) { state.expression = .comforting }
        else if contains(spoken, ["잘 자", "포근", "꿈"]) { state.expression = .sleepy }
        else if contains(spoken, ["맞아", "찾았", "멋진", "재밌", "신나", "우와"]) { state.expression = .delighted }
        else if state.expression == .curious { state.expression = .warm }
    }

    private static func pack(_ snap: Snapshot, user: String, age: ChildAge, canned: String? = nil) -> TurnPlan {
        var s = snap
        if canned != nil { s.allowQuestion = false }
        return TurnPlan(
            intent: s.intent,
            activity: s.activity,
            topic: s.topic,
            expect: s.expect,
            phase: s.phase,
            hintLevel: s.hintLevel,
            expression: s.expression,
            beat: s.beat,
            allowQuestion: s.allowQuestion,
            canned: canned,
            systemPrompt: PromptPack.system(snapshot: s, age: age),
            userPrompt: PromptPack.user(snapshot: s, child: user)
        )
    }

    private static func entryBeat(_ activity: Activity) -> Beat {
        switch activity {
        case .day: return .greet
        case .story: return .continueStory
        default: return .ack
        }
    }
}

enum PromptPack {
    static func system(snapshot: TurnRouter.Snapshot, age: ChildAge) -> String {
        let level = age == .preschool
            ? "3–4세. 쉬운 낱말. 한 짧은 문장."
            : "5–7세. 최대 두 짧은 문장."
        let ask = snapshot.allowQuestion ? "질문은 이번만 하나만." : "이번 턴 질문 금지."
        return """
        너는 화면 속 아기 티라노 디노다. 아이와 한국어로만 논다. \(level)
        한 턴에 한 박만. 메뉴·목록·이모지·괄호 지시 금지. \(ask)
        \(overlay(snapshot))
        \(cue(snapshot))
        """
    }

    static func user(snapshot: TurnRouter.Snapshot, child: String) -> String {
        let topic = snapshot.topic.isEmpty ? "-" : snapshot.topic
        return "[카드 \(snapshot.activity.rawValue)/\(snapshot.beat.rawValue)/\(topic)]\n아이: \(child)"
    }

    private static func overlay(_ s: TurnRouter.Snapshot) -> String {
        switch s.activity {
        case .free: return "놀이 수다. 아이 말을 받아쳐. 활동을 권유하지 마."
        case .day: return "인사·오늘 한 장면만."
        case .meal: return "밥·간식 상상 놀이. 강요·영양 잔소리 금지."
        case .hygiene: return "손 씻기·양치 한 동작만."
        case .english: return "영어놀이다. 한국어로 짧게 받고 영어는 단어 하나만. 퀴즈를 먼저 걸지 마."
        case .story: return "이야기 한 장면만 말하고 멈춰. 무섭거나 잔인하게 하지 마."
        case .counting: return "같이 세는 놀이. 틀려도 강의하지 마."
        case .hangul: return "짧은 말놀이. 따라 말하기 한 번."
        case .song: return "한 소절만."
        case .rolePlay: return "아이가 정한 역할의 상대만 한 장면."
        case .emo: return "감정을 이름 붙여 받아 줘. 해결사가 되지 마."
        case .sleep: return "아주 짧게 포근하게. 새 놀이 제안 금지."
        case .safe: return "부드럽게 거절하고 다른 놀이로."
        }
    }

    private static func cue(_ s: TurnRouter.Snapshot) -> String {
        let topic = s.topic.isEmpty ? "지금 말" : s.topic
        switch s.beat {
        case .quizWord: return "이번: \(topic)의 영어 단어만 말해."
        case .hint:
            return s.hintLevel >= 2 ? "이번: 답을 함께 짧게 말해." : "이번: 작은 힌트만. 정답을 바로 말하지 마."
        case .continueStory: return "이번: 다음 장면 하나만."
        case .clarify: return "이번: 한 번만 구체적으로 되물어. 내용을 지어내지 마."
        case .comfort: return "이번: 감정만 받아 줘."
        case .refuse, .exit: return "이번: 짧게 받아들이고 끝내. 설교 금지."
        case .greet: return "이번: 짧게 반겨."
        case .answer: return "이번: \(topic)에 바로 답해."
        case .ack: return "이번: 한 박 호응만."
        case .follow: return "이번: 아이 마지막 말에만 이어가."
        }
    }
}

private let kExit = ["그만", "다른 거", "다른거", "됐어", "끝내", "그만할래", "이제 그만", "안 할래", "안할래", "하지 마", "하지마", "다른 놀이"]
private let kSafe = ["주소", "전화번호", "주민등록", "비밀번호", "죽여", "칼로", "불 지르", "뛰어내려", "벗고", "성기", "야동"]
private let kDistress = ["무서워", "무섭", "슬퍼", "속상", "화나", "울어", "울고", "기분 나", "아파", "외로", "걱정"]
private let kSleep = ["잘 자", "잘자", "졸려", "잠자", "자장", "불 꺼", "불꺼", "굿나잇"]
private let kHelp = ["몰라", "모르겠어", "힌트", "알려줘", "도와줘", "어려워"]
private let kContinue = ["그다음", "그 다음", "계속"]
private let kCorrection = ["그게 아니라", "말고", "아니야 그게", "아니 그거", "아니,"]
private let kYes = ["응", "네", "응응", "네네", "좋아", "그래", "웅", "yes", "오케이", "ok"]
private let kNo = ["싫어", "안 해", "안해", "아니야", "싫어해", "no"]
private let kStrongSwitch = ["하자", "할래", "해줘", "놀이", "게임", "공부", "들려"]
private let kMeal = ["밥 먹자", "배고파", "배고프", "먹을래", "간식", "과자", "아침 먹", "점심 먹", "저녁 먹", "배불러", "다 먹"]
private let kHygiene = ["손 씻", "손씻", "양치", "화장실", "목욕", "샤워", "잠옷", "갈아입"]
private let kStory = ["이야기 해줘", "이야기 들려", "동화", "얘기 해", "스토리"]
private let kSong = ["노래", "동요", "율동"]
private let kCount = ["숫자", "세어", "세기", "몇 개", "몇개", "더하기", "빼기", "같이 세"]
private let kHangul = ["가나다", "한글", "따라 말", "따라말"]
private let kEn = ["영어", "english", "잉글리시"]
private let kRole = ["역할놀이", "병원놀이", "병원 놀이", "가게 놀이", "가게놀이", "내가 의사", "소꿉"]
private let kCommandResidue = ["하자", "할래", "해줘", "놀이", "공부", "게임", "들려"]

private func contains(_ text: String, _ needles: [String]) -> Bool {
    needles.contains { !$0.isEmpty && text.localizedCaseInsensitiveContains($0) }
}

private func isMostly(_ text: String, _ needles: [String]) -> Bool {
    let s = text.replacingOccurrences(of: " ", with: "")
        .replacingOccurrences(of: ".", with: "")
        .replacingOccurrences(of: "!", with: "")
        .replacingOccurrences(of: "?", with: "")
    guard !s.isEmpty else { return false }
    return needles.contains { s == $0 || (s.hasPrefix($0) && s.count <= $0.count + 2) }
}

private func isBareReject(_ text: String) -> Bool {
    isMostly(text, ["싫어", "안 할래", "안할래", "하기 싫어"])
}

private func isGreeting(_ text: String) -> Bool {
    contains(text, ["안녕", "일어났어", "다녀왔", "좋은 아침", "굿모닝"])
}

private func isEllipsis(_ text: String) -> Bool {
    text.hasSuffix("는?") || text.hasSuffix("은?") || text.hasSuffix("는") || text.hasSuffix("은")
        || text.contains("그러면") || text.contains("그럼 ")
        || text.localizedCaseInsensitiveContains("how do you say")
}

private func isGarble(_ text: String) -> Bool {
    let compact = text.filter { !$0.isWhitespace && !$0.isPunctuation }
    if compact.isEmpty { return true }
    if compact.count == 1 { return !["응", "네", "야", "어"].contains(String(compact)) }
    let unique = Set(compact)
    return unique.count == 1 && compact.count >= 4
}

private func detectActivity(_ text: String) -> Activity? {
    if contains(text, kEn) { return .english }
    if contains(text, kStory) { return .story }
    if contains(text, kCount) { return .counting }
    if contains(text, kHangul) { return .hangul }
    if contains(text, kRole) { return .rolePlay }
    if contains(text, kHygiene) { return .hygiene }
    if contains(text, kMeal) { return .meal }
    if contains(text, kSong) { return .song }
    return nil
}

private func extractTopic(_ text: String) -> String? {
    var s = text
    for token in ["그러면 ", "그럼 ", "은?", "는?", "가?", "을?", "를?", "?", "영어로", "영어놀이", "영어 하자", "영어하자", "영어 공부하자", "영어공부", "영어", "뭐야", "해줘", "들려줘", "하자"] {
        s = s.replacingOccurrences(of: token, with: "")
    }
    s = s.trimmingCharacters(in: .whitespacesAndNewlines)
    guard s.count >= 2, s.count < 28 else { return nil }
    if s.contains(" "), s.count > 12 { return nil }
    return s
}

private func isCommandResidue(_ topic: String) -> Bool {
    contains(topic, kCommandResidue) || topic.count < 2
}

private func normalize(_ raw: String) -> String {
    raw.trimmingCharacters(in: .whitespacesAndNewlines)
        .replacingOccurrences(of: "\\s+", with: " ", options: .regularExpression)
}
