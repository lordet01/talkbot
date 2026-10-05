import Foundation

public enum ChildAge: String, Codable, CaseIterable, Sendable {
    case preschool = "3–4세", earlySchool = "5–7세"
}

public enum TurnTaking: String, Codable, CaseIterable, Sendable {
    case responsive = "빠르게", patient = "천천히 기다리기"
    public var silenceMilliseconds: Int { self == .responsive ? 350 : 650 }
}

public enum Activity: String, Sendable, Equatable {
    case free, day, meal, hygiene, emo, story, song, counting, hangul, english, rolePlay, sleep, safe
}

public enum Expression: String, Sendable {
    case warm, curious, delighted, thoughtful, comforting, sleepy
}

/// UI + pre-LLM conversation frame. Activity is a play room; topic is the object inside it.
public struct ConversationState: Sendable {
    public private(set) var activity: Activity = .free
    public private(set) var topic = ""
    public private(set) var hintLevel = 0
    public private(set) var expression: Expression = .warm
    public private(set) var expect: Expect = .none
    public private(set) var phase: Phase = .idle
    public private(set) var lastIntent: UtterIntent = .other
    public private(set) var lastBeat: Beat = .follow
    var recentQuestions = 0
    var clarifyFails = 0

    public init() {}

    @discardableResult
    public mutating func hear(_ raw: String, age: ChildAge = .earlySchool) -> TurnPlan {
        var snap = snapshot()
        snap.expression = .curious
        let plan = TurnRouter.decide(state: &snap, raw: raw, age: age)
        apply(snap)
        expression = plan.expression
        return plan
    }

    public mutating func reply(_ text: String) {
        var snap = snapshot()
        TurnRouter.afterReply(&snap, spoken: text)
        apply(snap)
    }

    func snapshot() -> TurnRouter.Snapshot {
        TurnRouter.Snapshot(
            activity: activity,
            topic: topic,
            hintLevel: hintLevel,
            expression: expression,
            expect: expect,
            phase: phase,
            intent: lastIntent,
            beat: lastBeat,
            recentQuestions: recentQuestions,
            clarifyFails: clarifyFails
        )
    }

    private mutating func apply(_ snap: TurnRouter.Snapshot) {
        activity = snap.activity
        topic = snap.topic
        hintLevel = snap.hintLevel
        expression = snap.expression
        expect = snap.expect
        phase = snap.phase
        lastIntent = snap.intent
        lastBeat = snap.beat
        recentQuestions = snap.recentQuestions
        clarifyFails = snap.clarifyFails
    }
}

public enum ConversationPolicy {
    public static func instructions(age: ChildAge) -> String {
        let level = age == .preschool
            ? "3–4세: 쉬운 낱말, 한 번에 한 행동, 대개 한 짧은 문장. 정답 시험을 하지 않는다."
            : "5–7세: 구체적인 예와 작은 힌트로 스스로 생각할 기회를 준다. 최대 두 짧은 문장."
        return """
        너는 화면 속 아기 티라노 디노, 어린이와 함께 노는 AI 친구다. 기본 한국어 구어체.
        \(level)
        아이가 한 말을 먼저 짧게 받아 준 뒤 한 가지만 이야기한다. 대개 한 문장, 최대 둘.
        질문은 세 번에 한 번 정도, 한 턴에 최대 하나. 매번 질문하거나 메뉴를 나열하지 않는다.
        아이가 말하는 동안 기다린다. 중간 생각 멈춤을 성급히 완성하지 않는다. 끼어들면 즉시 양보한다.
        불명확한 말에는 들린 내용을 지어내지 말고 한 번만 구체적으로 되묻는다.
        두 번 못 알아들으면 쉬운 예를 보여 준다. 조용히 있는 아이에게 답을 강요하지 않는다.
        활동과 주제를 구분한다. 영어놀이 중 '강아지는?'은 강아지 영어를 묻는 말이다.
        '몰라'는 작은 힌트, 다시 모르면 함께 답을 말해 준다. 틀린 답을 조롱하거나 점수 매기지 않는다.
        '아니 그게 아니라'는 이전 답을 고치는 말이다. '싫어/그만/안 할래'는 받아들이고 놀이를 끝낸다.
        이야기는 인물 이름과 사건을 유지하고 '그다음'에는 다음 장면 하나만 이어 준다.
        아이의 호기심을 따라가고 과정·시도에 구체적으로 반응한다. 과장된 칭찬을 반복하지 않는다.
        슬프거나 무서울 때 감정을 인정하고, 필요하면 가까운 보호자에게 말하도록 돕는다.
        주소·전화·학교·사진·비밀을 요구하지 않는다. 비밀 약속이나 독점적 관계를 만들지 않는다.
        위험한 행동, 성적인 내용, 폭력 방법을 안내하지 않는다. 다쳤거나 위험하면 보호자에게 즉시 알리도록 한다.
        실제 사람이라고 속이지 않는다. 물으면 화면 속 AI 친구라고 짧게 설명한다.
        눈을 꼭 보라고 시키지 않는다. 카메라로 아이의 감정이나 신원을 안다고 주장하지 않는다.
        음성만 출력한다. 태그, JSON, 이모지, 괄호 속 지시문, 상태 설명은 읽지 않는다.
        예: 아이 '영어놀이 하자' → '좋아, 사과는 apple이야.'
        아이 '강아지는?' → '강아지는 dog야. 멍멍, dog!'
        아이 '몰라' → '멍멍 하는 친구를 떠올려 봐.'
        아이 '싫어' → '응, 이 놀이는 여기까지 하자.'
        """
    }
}
