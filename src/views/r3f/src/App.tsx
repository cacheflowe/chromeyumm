import * as THREE from "three";
import { useRef } from "react";
import { Canvas, useFrame } from "@react-three/fiber";
import { Environment } from "@react-three/drei";
import { EffectComposer, SSAO, Bloom } from "@react-three/postprocessing";
import { BlendFunction, KernelSize } from "postprocessing";
import { BallCollider, Physics, RigidBody, type RapierRigidBody } from "@react-three/rapier";

// ---------------------------------------------------------------------------
// Palette & geometry
// ---------------------------------------------------------------------------

const colors = [
  // "#e60000", // vivid red
  // "#008080", // deep teal
  "#0055cc", // royal blue
  // "#cc8800", // amber
  "#7b00e0", // electric purple
  // "#009688", // dark cyan
  // "#cc0066", // magenta
  // "#c43e00", // burnt orange
  // "#007744", // emerald
  // "#999", // deep violet
  "#111111", // deep violet
  // "#4400cc", // deep violet
];

const sphereGeometry = new THREE.SphereGeometry(1, 28, 28);
const pointerGeometry = new THREE.SphereGeometry(1, 32, 32);

type MaterialVariant = "glossy" | "satin" | "matte";
const variants: MaterialVariant[] = ["satin", "matte"];

interface BaubleEntry {
  scale: number;
  color: string;
  variant: MaterialVariant;
}

const baubles: BaubleEntry[] = [...Array(150)].map(() => ({
  scale: [0.75, 0.75, 1, 1, 1.25][Math.floor(Math.random() * 5)],
  color: colors[Math.floor(Math.random() * colors.length)],
  variant: variants[Math.floor(Math.random() * variants.length)],
}));

// Pre-create materials per color+variant so they're shared across baubles.
const materialKey = (color: string, variant: MaterialVariant) => `${color}_${variant}`;
const materialMap = new Map<string, THREE.MeshStandardMaterial>();

const variantProps: Record<MaterialVariant, { roughness: number; metalness: number; emissiveIntensity: number }> = {
  glossy: { roughness: 0.15, metalness: 0.3, emissiveIntensity: 0.1 },
  satin: { roughness: 0.45, metalness: 0.05, emissiveIntensity: 0.06 },
  matte: { roughness: 0.85, metalness: 0.0, emissiveIntensity: 0.03 },
};

for (const c of colors) {
  for (const v of variants) {
    const p = variantProps[v];
    materialMap.set(
      materialKey(c, v),
      new THREE.MeshStandardMaterial({
        color: c,
        emissive: c,
        emissiveIntensity: p.emissiveIntensity,
        roughness: p.roughness,
        metalness: p.metalness,
      }),
    );
  }
}

// ---------------------------------------------------------------------------
// Bauble — a colored sphere pulled toward the origin by a spring impulse
// ---------------------------------------------------------------------------

function Bauble({
  scale,
  color,
  variant,
  vec = new THREE.Vector3(),
  r = THREE.MathUtils.randFloatSpread,
}: BaubleEntry & { vec?: THREE.Vector3; r?: (range: number) => number }) {
  const api = useRef<RapierRigidBody>(null);

  useFrame((_state, delta) => {
    delta = Math.min(0.1, delta);
    if (!api.current) return;
    vec.copy(api.current.translation()).normalize().multiplyScalar(-1);
    api.current.applyImpulse(
      { x: vec.x * 50 * delta * scale, y: vec.y * 150 * delta * scale, z: vec.z * 50 * delta * scale },
      true,
    );
  });

  return (
    <RigidBody
      linearDamping={0.65}
      angularDamping={0.15}
      friction={0.2}
      position={[r(20), r(20) - 25, r(20) - 10]}
      ref={api}
      colliders={false}
    >
      <BallCollider args={[scale]} />
      <mesh
        castShadow
        receiveShadow
        scale={scale}
        geometry={sphereGeometry}
        material={materialMap.get(materialKey(color, variant))}
      />
    </RigidBody>
  );
}

// ---------------------------------------------------------------------------
// AutoPointer — kinematic body that traces a smooth multi-frequency path
// through the bauble cluster, rendered as a glowing emissive sphere with
// a point light that illuminates nearby baubles.
// ---------------------------------------------------------------------------

function AutoPointer() {
  const ref = useRef<RapierRigidBody>(null);
  const meshRef = useRef<THREE.Mesh>(null);
  const lightRef = useRef<THREE.PointLight>(null);
  const timeRef = useRef(0);
  const pos = useRef(new THREE.Vector3());

  useFrame((_state, delta) => {
    timeRef.current += delta;
    const t = timeRef.current;

    const ampX = Math.sin(t * 3.3) * 3;
    const ampY = Math.cos(t * 2.4) * 6;
    const ampZ = Math.sin(t * 3.5) * 2;

    const tx = ampX;
    const ty = ampY;
    const tz = ampZ;

    const smoothing = 1 - Math.exp(-3.5 * delta);
    pos.current.lerp({ x: tx, y: ty, z: tz }, smoothing);

    ref.current?.setNextKinematicTranslation(pos.current);

    // Sync the visible mesh and light to the physics body position.
    if (meshRef.current) {
      meshRef.current.position.copy(pos.current);
    }
    if (lightRef.current) {
      lightRef.current.position.copy(pos.current);
    }
  });

  return (
    <>
      <RigidBody position={[0, 0, 0]} type="kinematicPosition" colliders={false} ref={ref}>
        <BallCollider args={[2.5]} />
      </RigidBody>
      {/* Visible glowing sphere — positioned outside the RigidBody so it */}
      {/* doesn't interfere with physics but tracks the same position.     */}
      <mesh ref={meshRef} geometry={pointerGeometry} scale={1.8}>
        <meshPhysicalMaterial
          color="#ffffff"
          emissive="#ffffff"
          emissiveIntensity={10}
          roughness={0.02}
          metalness={0.9}
          clearcoat={1}
          clearcoatRoughness={0}
          reflectivity={1}
          envMapIntensity={4}
        />
      </mesh>
      <pointLight ref={lightRef} color="#ffffff" intensity={120} distance={30} decay={2} />
    </>
  );
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

export function App() {
  return (
    <Canvas
      shadows={{ type: THREE.PCFShadowMap }}
      gl={{ alpha: true, stencil: false, antialias: false }}
      camera={{ position: [0, 0, 20], fov: 32.5, near: 1, far: 100 }}
      onCreated={(state) => {
        state.gl.toneMappingExposure = 0.75;
      }}
    >
      <ambientLight intensity={0.4} />
      <spotLight
        position={[20, 20, 25]}
        penumbra={1}
        angle={0.2}
        color="white"
        castShadow
        shadow-mapSize={[2048, 2048]}
        shadow-bias={-0.0001}
        intensity={2}
      />
      <directionalLight
        position={[0, 5, -4]}
        intensity={3}
        castShadow
        shadow-mapSize={[2048, 2048]}
        shadow-camera-left={-15}
        shadow-camera-right={15}
        shadow-camera-top={15}
        shadow-camera-bottom={-15}
        shadow-bias={-0.0001}
      />
      <directionalLight position={[0, -15, 0]} intensity={4} color="red" />

      <Physics gravity={[0, 0, 0]}>
        <AutoPointer />
        {baubles.map((props, i) => (
          <Bauble key={i} {...props} />
        ))}
      </Physics>

      <Environment preset="studio" />
      <EffectComposer>
        <SSAO
          blendFunction={BlendFunction.MULTIPLY}
          samples={32}
          rings={8}
          radius={5}
          intensity={100}
          luminanceInfluence={0.2}
          bias={0.025}
          worldDistanceThreshold={20}
          worldDistanceFalloff={5}
          worldProximityThreshold={0.5}
          worldProximityFalloff={0.3}
          color={new THREE.Color(0, 0, 0)}
        />
        {/* <Bloom
          intensity={0.2}
          luminanceThreshold={0.3}
          luminanceSmoothing={0.3}
          mipMap={true}
          resolutionX={1024}
          resolutionY={1024}
          kernelSize={KernelSize.SMALL}
        /> */}
      </EffectComposer>
    </Canvas>
  );
}
