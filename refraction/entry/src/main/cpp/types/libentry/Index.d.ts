declare namespace libentry {
    /** Initialize OpenGL/EGL rendering context. */
    function initGraphics(resourceManager: object, surfaceId: string, fileDir: string): void;

    /** Render one frame. */
    function renderFrame(): void;

    /** Forward touch event to native, type: 0=Down, 1=Up, 2=Move. */
    function onTouch(x: number, y: number, type: number): void;

    function setInputViewport(width: number, height: number): void;

    function releaseResource(): void;
}
export default libentry;