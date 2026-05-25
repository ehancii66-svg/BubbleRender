import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { common } from '@kit.AbilityKit';

const DOMAIN = 0x0000;

@Entry
@Component
struct Index {
    private xcomponentId: string = 'xcomponent'
    private xComponentController: XComponentController = new XComponentController()

    private renderTimerId: number = -1

    @State private compX: number = 0;
    @State private compY: number = 0;
    @State private compW: number = 0;
    @State private compH: number = 0;

    private startRenderLoop(): void {
        if (this.renderTimerId >= 0) {
            return
        }
        this.renderTimerId = setInterval(() => {
            try {
                testNapi.renderFrame()
            } catch (err) {
                hilog.error(DOMAIN, 'Index', 'renderFrame failed: %{public}s', JSON.stringify(err))
            }
        }, 16)
        hilog.info(DOMAIN, 'Index', 'render loop started')
    }

    private stopRenderLoop(): void {
        if (this.renderTimerId < 0) {
            return
        }
        clearInterval(this.renderTimerId)
        this.renderTimerId = -1
        hilog.info(DOMAIN, 'Index', 'render loop stopped')
    }

    private mapAndDispatchTouch(event: TouchEvent): void {
        let points = event.touches
        if (!points || points.length === 0) {
            points = event.changedTouches
        }
        if (!points || points.length === 0) {
            return
        }

        const touch = points[0]
        let touchType = 2
        switch (event.type) {
            case TouchType.Down:
                touchType = 0
                break
            case TouchType.Up:
                touchType = 1
                break
            case TouchType.Move:
                touchType = 2
                break
        }
        const hasArea = this.compW > 0 && this.compH > 0
        const localX = hasArea ? (touch.x - this.compX) : touch.x
        const localY = hasArea ? (touch.y - this.compY) : touch.y

        const inComp = hasArea ? (localX >= 0 && localX <= this.compW && localY >= 0 && localY <= this.compH) : true
        if (!inComp && touchType !== 1) {
            return
        }

        hilog.info(
            DOMAIN,
            'Index',
            `onTouch map global=(${touch.x},${touch.y}) local=(${localX},${localY}) type=${touchType} inComp=${inComp}`
        )

        try {
            testNapi.onTouch(localX, localY, touchType)
        } catch (err) {
            hilog.error(DOMAIN, 'Index', 'call onTouch failed: %{public}s', JSON.stringify(err))
        }
    }

    aboutToAppear(): void {
        this.startRenderLoop()
    }

    aboutToDisappear(): void {
        this.stopRenderLoop()
        testNapi.releaseResource()
    }

    build() {
        Column() {
            XComponent({
                id: this.xcomponentId,
                type: XComponentType.SURFACE,
                libraryname: 'entry',
                controller: this.xComponentController
            })
                .onLoad(() => {
                    const context = getContext(this) as common.UIAbilityContext
                    let fileDir = context.filesDir
                    hilog.info(0x0000, 'Test', `filesDir = ${fileDir}`)
                    const surfaceId = this.xComponentController.getXComponentSurfaceId()
                    testNapi.initGraphics(context.resourceManager, surfaceId, fileDir)
                    if (this.compW > 0 && this.compH > 0) {
                        testNapi.setInputViewport(this.compW, this.compH)
                    }
                    this.startRenderLoop()
                })
                .onAreaChange((_oldValue: Area, newValue: Area) => {
                    this.compX = Number(newValue.position.x)
                    this.compY = Number(newValue.position.y)
                    this.compW = Number(newValue.width)
                    this.compH = Number(newValue.height)
                    hilog.info(DOMAIN, 'Index', `XComponent area x=${this.compX}, y=${this.compY}, w=${this.compW}, h=${this.compH}`)
                    try {
                        testNapi.setInputViewport(this.compW, this.compH)
                    } catch (err) {
                        hilog.error(DOMAIN, 'Index', 'setInputViewport failed: %{public}s', JSON.stringify(err))
                    }
                })
                .width('100%')
                .height('100%')
        }
        .onTouch((event: TouchEvent) => {
            this.mapAndDispatchTouch(event)
        })
        .height('100%')
    }
}
